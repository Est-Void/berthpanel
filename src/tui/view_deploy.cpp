#include "view_deploy.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <utility>

#include "../util/strutil.h"
#include "view_catalog.h"

namespace tui {

namespace {

// Exact matches on the FIRST token: a line that merely mentions "build" is not
// the build stage.
DeployStage stage_of(const std::string_view first) {
  if (first == "fetch") {
    return DeployStage::kFetch;
  }
  if (first == "build") {
    return DeployStage::kBuild;
  }
  if (first == "start" || first == "unit") {
    return DeployStage::kStart;
  }
  if (first == "done") {
    return DeployStage::kDone;
  }
  // The one failure that must never read as a success: the unit is live and
  // the panel does not know about it (§11).
  if (berth::starts_with(first, "REGISTRY")) {
    return DeployStage::kRegistryFailed;
  }
  return DeployStage::kOther;
}

// "fetch berthpanel-memos@<sha> port 8080" names the service in field 1,
// "REGISTRY WRITE FAILED <name>: <why>" in field 3. A token carrying '@' is a
// pinned reference, not a unit name.
std::string service_of(const std::vector<std::string>& fields,
                       const DeployStage stage) {
  const std::size_t index = stage == DeployStage::kRegistryFailed ? 3 : 1;
  if (fields.size() <= index) {
    return {};
  }
  std::string_view token = berth::trim(fields[index]);
  // The colon belongs to the log line's punctuation, not to the unit name.
  if (!token.empty() && token.back() == ':') {
    token = token.substr(0, token.size() - 1);
  }
  const std::size_t at = token.find('@');
  return std::string(at == std::string_view::npos ? token
                                                  : token.substr(0, at));
}

}
std::string_view deploy_stage_label(const DeployStage stage) {
  switch (stage) {
    case DeployStage::kFetch:
      return "fetch";
    case DeployStage::kBuild:
      return "build";
    case DeployStage::kStart:
      return "unit + start";
    case DeployStage::kDone:
      return "done";
    case DeployStage::kRegistryFailed:
      return "REGISTRY WRITE FAILED";
    case DeployStage::kOther:
      return "log";
  }
  return "log";
}

DeployLogLine parse_deploy_log_line(const std::string_view raw) {
  DeployLogLine line;
  // Folded, never dropped: a line must not become two records on screen, and
  // neither may it vanish.
  std::string folded(raw);
  std::replace(folded.begin(), folded.end(), '\n', ' ');
  line.text = berth::trim(folded);

  // deploylog_append stamps "<ISO-8601> " in front of every line, and the
  // stamp has no spaces, so the first space separates it. A line without one is
  // hand-written and parses with an empty timestamp. `text` keeps the stamp:
  // on the tail pane the timestamp is information, not noise.
  const std::size_t space = line.text.find(' ');
  std::string_view body = line.text;
  if (space != std::string::npos) {
    const std::string_view head = std::string_view(line.text).substr(0, space);
    const bool looks_like_stamp = head.size() >= 19 && head[4] == '-' &&
                                  head[7] == '-' && head[10] == 'T';
    if (looks_like_stamp) {
      line.timestamp = std::string(head);
      body = std::string_view(line.text).substr(space + 1);
    }
  }

  const std::vector<std::string> fields = berth::split(body, " ");
  if (fields.empty()) {
    return line;
  }
  line.stage = stage_of(fields[0]);
  line.service = service_of(fields, line.stage);
  return line;
}

DeployStage parse_deploy_stage(const std::string_view raw) {
  return parse_deploy_log_line(raw).stage;
}

std::vector<std::string> deploy_stage_lines(const DeployStage current,
                                            const bool finished,
                                            const std::string& failure) {
  // The whole ladder is drawn so a deploy stuck in "build" also shows what it
  // has NOT reached.
  constexpr std::array<DeployStage, 5> kLadder{
      DeployStage::kFetch, DeployStage::kBuild, DeployStage::kStart,
      DeployStage::kDone, DeployStage::kRegistryFailed};
  const std::size_t reached = static_cast<std::size_t>(current);

  std::vector<std::string> lines;
  for (const DeployStage stage : kLadder) {
    const bool is_reached = static_cast<std::size_t>(stage) <= reached;
    // A verdict, not a rung: drawn on a successful deploy it would read as a
    // failure.
    if (stage == DeployStage::kRegistryFailed && !is_reached) {
      continue;
    }
    std::string marker = "  ";
    if (stage == DeployStage::kRegistryFailed) {
      marker = is_reached ? "!!" : "  ";
    } else {
      marker = is_reached ? "ok" : "  ";
    }
    std::string text = marker;
    text += "  ";
    text += std::string(deploy_stage_label(stage));
    if (stage == current) {
      text += finished ? "  (last stage reached)" : "  ...";
    }
    lines.push_back(std::move(text));
  }
  if (current == DeployStage::kOther && !finished) {
    lines.push_back("  (no stage logged yet)");
  }
  if (!failure.empty()) {
    lines.push_back("  " + failure);
  }
  return lines;
}

berth::Result<std::uint16_t> parse_port_field(const std::string_view field) {
  const std::string_view typed = berth::trim(field);
  if (typed.empty()) {
    return std::uint16_t{0};  // 0 = ask the recipe, then pick a free port
  }
  unsigned long value = 0;
  for (const char digit : typed) {
    if (digit < '0' || digit > '9') {
      return berth::Error(
          "port: '" + std::string(typed) +
          "' is not a number — clear the field for a free port, "
          "or type a port in 1-65535");
    }
    value = value * 10 + static_cast<unsigned long>(digit - '0');
    if (value > 65535UL) {
      return berth::Error("port: '" + std::string(typed) +
                          "' is above 65535 — a TCP port is 16 bits");
    }
  }
  if (value == 0UL) {
    return berth::Error(
        "port: 0 is not a port you can ask for — leave the field empty for "
        "a free port, or type one in 1-65535");
  }
  return static_cast<std::uint16_t>(value);
}

std::vector<std::string> deploy_idle_lines(const berth::Recipe& recipe,
                                           const std::uint16_t chosen_port,
                                           const bool have_choice) {
  std::vector<std::string> lines;
  lines.push_back("ready: " + recipe.service_name + "  (" +
                  recipe.owner_repo() + " @" + recipe.commit.substr(0, 7) +
                  ")");
  if (have_choice) {
    lines.push_back("port: " + std::to_string(chosen_port) +
                    "  (typed above — Enter deploys here)");
  } else if (recipe.port != 0) {
    lines.push_back("port: " + std::to_string(recipe.port) +
                    "  (the recipe's own default; leave the field empty to "
                    "keep it)");
  } else {
    lines.push_back(
        "port: free  (the recipe asks for none, so a free one is picked at "
        "deploy time)");
  }
  lines.push_back(
      "the deploy lock is taken by the engine, so a deploy started elsewhere "
      "is refused here too");
  return lines;
}

std::vector<KeyHint> deploy_key_hints(const bool running) {
  if (running) {
    return {{"c", "cancel the deploy (the journal entry stays for __cleanup)"},
            {"q", "quit — the deploy is cancelled and unwound first"}};
  }
  return {
      {"Enter", "deploy the selected recipe at the typed port"},
      {"i", "create a new service (url, ref, port + the eight fields)"},
      {"e", "recipe details (pin, tools, build steps, start command)"},
      {"E", "edit the selected USER recipe (builtin: make an override first)"},
      {"x", "remove the selected USER recipe (builtin: nothing to remove)"},
      {"r", "reload the catalog"},
      {"\xe2\x86\x91\xe2\x86\x93", "select a recipe"},
  };
}

std::string_view create_field_label(const CreateField field) {
  switch (field) {
    case CreateField::kUrl:
      return "url";
    case CreateField::kRef:
      return "ref";
    case CreateField::kDeployPort:
      return "deploy port";
    case CreateField::kStartCmd:
      return "start_cmd";
    case CreateField::kBind:
      return "bind";
    case CreateField::kHealthPath:
      return "health_path";
    case CreateField::kDisplayName:
      return "display_name";
    case CreateField::kDescription:
      return "description";
    case CreateField::kAppPort:
      return "app port";
    case CreateField::kMemoryMax:
      return "memory_max";
    case CreateField::kTasksMax:
      return "tasks_max";
    case CreateField::kNone:
      return "";
  }
  return "";
}

CreateField next_create_field(const CreateField field) {
  switch (field) {
    case CreateField::kNone:  // entering insert mode starts at the url
    case CreateField::kTasksMax:
      return CreateField::kUrl;
    case CreateField::kUrl:
      return CreateField::kRef;
    case CreateField::kRef:
      return CreateField::kDeployPort;
    case CreateField::kDeployPort:
      return CreateField::kStartCmd;
    case CreateField::kStartCmd:
      return CreateField::kBind;
    case CreateField::kBind:
      return CreateField::kHealthPath;
    case CreateField::kHealthPath:
      return CreateField::kDisplayName;
    case CreateField::kDisplayName:
      return CreateField::kDescription;
    case CreateField::kDescription:
      return CreateField::kAppPort;
    case CreateField::kAppPort:
      return CreateField::kMemoryMax;
    case CreateField::kMemoryMax:
      return CreateField::kTasksMax;
  }
  return CreateField::kUrl;
}

CreateField create_field_at(const std::size_t index) {
  switch (index) {
    case 0:
      return CreateField::kUrl;
    case 1:
      return CreateField::kRef;
    case 2:
      return CreateField::kDeployPort;
    case 3:
      return CreateField::kStartCmd;
    case 4:
      return CreateField::kBind;
    case 5:
      return CreateField::kHealthPath;
    case 6:
      return CreateField::kDisplayName;
    case 7:
      return CreateField::kDescription;
    case 8:
      return CreateField::kAppPort;
    case 9:
      return CreateField::kMemoryMax;
    case 10:
      return CreateField::kTasksMax;
    default:
      break;
  }
  std::fputs("tui: invariant violated: create field index out of range\n",
             stderr);
  std::abort();
}

std::vector<KeyHint> create_key_hints(const CreateField focus,
                                      const bool creating) {
  if (creating) {
    return {{"c", "cancel — nothing is created or stored"}, {"q", "quit"}};
  }
  if (focus != CreateField::kNone) {
    return {{"Tab", "next field"},
            {"Enter", "create the recipe from these fields, then review it"},
            {"Esc", "leave the fields — the form's own keys work again"}};
  }
  return {{"i", "type a field"},
          {"Enter", "create the recipe from these fields, then review it"},
          {"Esc", "close the form — nothing was created or stored"},
          {"q", "quit"}};
}

std::vector<std::string> draft_review_lines(const berth::DraftResult& draft) {
  const berth::Recipe& recipe = draft.draft;
  std::vector<std::string> lines;
  lines.push_back("draft " + recipe.service_name + "  (" + recipe.language +
                  ", " + recipe.res.memory_max + "/" +
                  std::to_string(recipe.res.tasks_max) + ")");
  lines.push_back("  repo:  " + draft.owner_repo + " @ " + draft.commit);
  lines.push_back("  start: " + recipe.start_cmd);
  lines.push_back("  build: " +
                  (recipe.build_steps.empty()
                       ? std::string("(a prebuilt binary — no build)")
                       : render_step(recipe.build_steps.front())));
  for (const berth::ToolStatus& tool : draft.tools) {
    lines.push_back(std::string("  ") + (tool.present ? "ok   " : "MISS ") +
                    tool.name +
                    (tool.present ? "  " + tool.version
                                  : "  — install it before this can build"));
  }
  for (const std::string& warning : draft.warnings) {
    // Verbatim, not counted: "1 warning" says nothing about WHICH (§11).
    lines.push_back("  warning: " + warning);
  }
  lines.push_back("  heuristics, not truth — check these before deploying");
  return lines;
}

// One line only: the full question is a MODAL on d/s, and rendering both would
// look like the screen already asked and is waiting.
std::vector<std::string> draft_clash_notice_lines(
    const berth::DraftClash& clash);

std::vector<std::string> draft_clash_lines(const berth::DraftClash& clash,
                                           const std::string_view new_commit) {
  std::vector<std::string> lines;
  lines.push_back("overwrite " + clash.service_name + "?");
  lines.push_back("");
  lines.push_back(
      std::string("  now:   ") +
      (clash.in_catalog ? "the builtin catalog" : "a stored user recipe") +
      (clash.existing_commit.empty()
           ? std::string("  (unpinned)")
           : "  pinned " + clash.existing_commit.substr(0, 7)));
  lines.push_back("  then:  the draft above, pinned " +
                  std::string(new_commit.substr(0, 7)));
  if (!clash.existing_path.empty()) {
    lines.push_back("  file:  " + clash.existing_path.string());
  }
  lines.push_back("");
  lines.push_back("  y  yes, overwrite and store the draft");
  lines.push_back("  n  no, leave it alone");
  return lines;
}

std::vector<std::string> draft_clash_notice_lines(
    const berth::DraftClash& clash) {
  if (!clash.exists) {
    return {};
  }
  std::string line = "  storing it will ask first: " + clash.service_name +
                     " is already there";
  if (clash.in_catalog) {
    line += " (builtin catalog)";
  }
  if (!clash.existing_commit.empty()) {
    line += ", pinned " + clash.existing_commit.substr(0, 7);
  }
  return {line};
}

std::vector<std::string> draft_stored_lines(
    const std::filesystem::path& recipe_path) {
  return {
      "saved: " + recipe_path.string(),
      "selected in the list above — Esc closes this draft, then Enter deploys "
      "it",
      "it is on disk before anything deploys — the draft you deploy is the "
      "draft you can read",
  };
}

std::vector<KeyHint> draft_key_hints(const bool running, const bool ready) {
  if (running) {
    return {{"c", "cancel — the draft is thrown away and nothing is stored"},
            {"q", "quit"}};
  }
  if (ready) {
    return {{"d", "store the draft and deploy it"},
            {"s", "store the draft only (then select it and press Enter)"},
            {"Esc", "discard the draft — Enter, e and x work again"},
            {"i", "paste another URL"},
            {"\xe2\x86\x91\xe2\x86\x93", "select a recipe"}};
  }
  return {
      {"i", "paste a GitHub URL to deploy anything (BYO)"},
      {"Enter", "deploy the selected recipe at the typed port"},
      {"e", "recipe details"},
      {"r", "reload the catalog"},
      {"\xe2\x86\x91\xe2\x86\x93", "select a recipe"},
  };
}

std::vector<std::string> deploy_outcome_lines(
    const berth::DeployOutcome& outcome) {
  std::vector<std::string> lines;
  lines.push_back("deployed " + outcome.service_name +
                  " — the unit is active "
                  "and registered");
  lines.push_back("  port: " + std::to_string(outcome.port));
  lines.push_back("  unit: " + outcome.unit_path.string());
  lines.push_back("  data: " + outcome.work_dir.string());
  lines.push_back("  uninstall leaves the data dir in place, on purpose");
  return lines;
}

std::vector<std::string> deploy_cancelled_lines(const std::string& service) {
  std::vector<std::string> lines;
  lines.push_back("cancelled " + service);
  lines.push_back(
      "the journal entry stays on purpose (§6.7): an interrupted deploy is "
      "exactly what __cleanup sweeps");
  lines.push_back("  berthpanel __cleanup      (picks it up now)");
  lines.push_back("  or the cleanup timer will, on its own schedule");
  lines.push_back(
      "nothing was deleted: whatever the build produced is still in "
      "the work dir");
  return lines;
}

std::vector<std::string> deploy_error_lines(const std::string& error) {
  if (error.empty()) {
    return {};
  }
  std::vector<std::string> lines;
  lines.push_back("deploy failed:");
  for (const std::string& part : berth::split(error, "\n")) {
    lines.push_back("  " + part);
  }
  return lines;
}

DeployVerdict deploy_verdict(const bool finished, const bool cancelled,
                             const std::string& error) {
  DeployVerdict verdict;
  verdict.cancelled = cancelled;
  // A cancellation carries an engine error only to unwind; it is not a failure
  // and must not render as one.
  verdict.failed = !cancelled && !error.empty();
  verdict.success = finished && !cancelled && error.empty();
  return verdict;
}

}
