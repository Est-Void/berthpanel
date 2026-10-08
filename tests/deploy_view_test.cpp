// Tests for the Deploy view model (stage 6.3). The parser is the load-bearing
// piece: deploy.log is written by the ENGINE, so the screen learns where a
// deploy is by reading the log — and "a log line the panel refuses to show" is
// a worse failure than "a stage the panel does not recognise".

#include "tui/view_deploy.h"

#include <cstdint>
#include <string>
#include <vector>

#include "berth/wizard.h"
#include "runner.h"
#include "tui/ring.h"

namespace {

// One line exactly as deploylog_append writes it: ISO-8601 stamp, space, text.
std::string stamped(const std::string& text) {
  return "2026-10-08T12:34:56.789Z " + text;
}

berth::Recipe sample_recipe() {
  berth::Recipe r;
  r.owner = "usememos";
  r.name = "memos";
  r.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  r.language = "go";
  r.service_name = "berthpanel-memos";
  r.display_name = "Memos";
  r.description = "self-hosted note taking";
  r.build_steps = {{"go", "build", "-o", "memos", "./cmd/memos"}};
  r.tools = {"go", "git"};
  r.res = {"512M", 300};
  r.binary_name = "memos";
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port}";
  r.port = 0;
  return r;
}

bool contains(const std::vector<std::string>& haystack,
              const std::string_view needle) {
  for (const std::string& line : haystack) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(deploy_stage_parser_names_every_stage_the_engine_logs) {
  // The fixtures below are the log lines run_deploy actually writes, so a
  // change to the engine's wording breaks this test instead of the screen.
  CHECK(tui::parse_deploy_stage(stamped(
            "fetch berthpanel-memos@2b2192d4e153bd04f1d325b60fd880cf00d68b01 "
            "port 8234")) == tui::DeployStage::kFetch);
  CHECK(tui::parse_deploy_stage(stamped("build berthpanel-memos port 8234")) ==
        tui::DeployStage::kBuild);
  CHECK(tui::parse_deploy_stage(stamped("start berthpanel-memos")) ==
        tui::DeployStage::kStart);
  CHECK(tui::parse_deploy_stage(stamped("done berthpanel-memos port 8234")) ==
        tui::DeployStage::kDone);
}

TEST(deploy_stage_parser_surfaces_a_registry_write_failure) {
  // The one outcome that must never read as a success (§11): the unit is live
  // and the panel does not know about it.
  const tui::DeployLogLine line = tui::parse_deploy_log_line(stamped(
      "REGISTRY WRITE FAILED berthpanel-memos: state.json is a directory"));
  CHECK(line.stage == tui::DeployStage::kRegistryFailed);
  CHECK(line.service == "berthpanel-memos");
  CHECK(tui::deploy_stage_label(tui::DeployStage::kRegistryFailed) ==
        "REGISTRY WRITE FAILED");
}

TEST(deploy_stage_parser_separates_the_timestamp_and_the_service) {
  const tui::DeployLogLine line = tui::parse_deploy_log_line(
      stamped("fetch berthpanel-memos@2b2192d4e153bd04f1d325b60fd880cf00d68b01 "
              "port 8234"));
  CHECK(line.timestamp == "2026-10-08T12:34:56.789Z");
  CHECK(line.stage == tui::DeployStage::kFetch);
  // The pin is glued to the name in this line; the service is the unit stem.
  CHECK(line.service == "berthpanel-memos");
  CHECK(line.text.find("port 8234") != std::string::npos);
}

TEST(deploy_stage_parser_keeps_a_line_it_cannot_classify) {
  // A sweep line, and a hand-written line with no timestamp at all. Both must
  // survive to the screen: an unreadable log line that vanishes is silence
  // where the user is debugging (§11).
  const tui::DeployLogLine sweep = tui::parse_deploy_log_line(
      stamped("cleanup failed for berthpanel-other: unit still active"));
  CHECK(sweep.stage == tui::DeployStage::kOther);
  CHECK(sweep.timestamp == "2026-10-08T12:34:56.789Z");
  CHECK(sweep.text.find("cleanup failed") != std::string::npos);

  const tui::DeployLogLine bare =
      tui::parse_deploy_log_line("done berthpanel-x");
  CHECK(bare.stage == tui::DeployStage::kDone);
  CHECK(bare.timestamp.empty());
  CHECK(bare.service == "berthpanel-x");

  const tui::DeployLogLine empty = tui::parse_deploy_log_line("");
  CHECK(empty.stage == tui::DeployStage::kOther);
  CHECK(empty.text.empty());
}

TEST(deploy_stage_parser_never_splits_one_record_into_two) {
  // A newline inside a line would render as two records on the tail pane.
  const tui::DeployLogLine line =
      tui::parse_deploy_log_line("done berthpanel-memos\nport 8234");
  CHECK(line.text.find('\n') == std::string::npos);
  CHECK(line.stage == tui::DeployStage::kDone);
}

TEST(deploy_stage_lines_show_reached_pending_and_the_failure) {
  const std::vector<std::string> fetching =
      tui::deploy_stage_lines(tui::DeployStage::kFetch, false, "");
  CHECK(contains(fetching, "ok"));
  CHECK(contains(fetching, "fetch"));
  CHECK(contains(fetching, "..."));
  // Nothing past the current stage claims to be done.
  CHECK(!contains(fetching, "done  (last stage reached)"));

  const std::vector<std::string> finished =
      tui::deploy_stage_lines(tui::DeployStage::kDone, true, "");
  CHECK(contains(finished, "(last stage reached)"));

  // A failure line is carried verbatim: the screen's own error box may wrap it,
  // but the ladder must not swallow it.
  const std::vector<std::string> failed = tui::deploy_stage_lines(
      tui::DeployStage::kBuild, false, "build failed: exit 1");
  CHECK(contains(failed, "build failed: exit 1"));

  // Before anything is logged, the ladder says so rather than showing nothing.
  CHECK(contains(tui::deploy_stage_lines(tui::DeployStage::kOther, false, ""),
                 "no stage logged yet"));
}

TEST(deploy_port_field_treats_empty_as_auto_and_refuses_nonsense) {
  // Empty is the recipe's own default or a free port — never a hardcoded one.
  CHECK(tui::parse_port_field("").value() == 0);
  CHECK(tui::parse_port_field("   ").value() == 0);
  CHECK(tui::parse_port_field("8234").value() == 8234);
  CHECK(tui::parse_port_field(" 8234 ").value() == 8234);
  CHECK(tui::parse_port_field("65535").value() == 65535);

  // A typo is a loud refusal, not a silent 0 that deploys somewhere else.
  for (const std::string_view bad :
       {"0", "65536", "-1", "80O", "8080a", "8 080", "0x1f90"}) {
    const berth::Result<std::uint16_t> parsed = tui::parse_port_field(bad);
    CHECK(parsed.is_err());
    CHECK(!parsed.error().ctx().empty());
  }
}

// The port field is behind an explicit insert mode, so it takes text and the
// PARSER refuses nonsense loudly — the rule that protects 'q' now lives in the
// focus handling, and the parse test above is what protects the port.
TEST(deploy_port_field_refuses_letters_loudly_instead_of_dropping_them) {
  const berth::Result<std::uint16_t> parsed = tui::parse_port_field("8O3O");
  CHECK(parsed.is_err());
  CHECK(parsed.error().ctx().find("8O3O") != std::string::npos);
  CHECK(parsed.error().ctx().find("not a number") != std::string::npos);
}

TEST(draft_fields_cycle_url_ref_port) {
  // The cycle is the whole focus model: 'i' enters at the url, Tab walks
  // forward, and no field can be reached without passing through the others.
  CHECK(tui::draft_field_label(tui::DraftField::kUrl) == "url");
  CHECK(tui::draft_field_label(tui::DraftField::kRef) == "ref");
  CHECK(tui::draft_field_label(tui::DraftField::kPort) == "port");
  CHECK(tui::draft_field_label(tui::DraftField::kNone).empty());

  CHECK(tui::next_draft_field(tui::DraftField::kNone) == tui::DraftField::kUrl);
  CHECK(tui::next_draft_field(tui::DraftField::kUrl) == tui::DraftField::kRef);
  CHECK(tui::next_draft_field(tui::DraftField::kRef) == tui::DraftField::kPort);
  CHECK(tui::next_draft_field(tui::DraftField::kPort) == tui::DraftField::kUrl);
}

TEST(draft_field_lines_mark_the_focused_one_and_keep_the_columns) {
  // Labels only: the values are the Inputs' own render (they own the cursor),
  // so a field gaining a value must not move the marker.
  const std::vector<std::string> none =
      tui::draft_field_lines(tui::DraftField::kNone);
  CHECK(none.size() == 3);
  CHECK(none[0].find("url") != std::string::npos);
  CHECK(none[1].find("ref") != std::string::npos);
  CHECK(none[2].find("port") != std::string::npos);
  // No field focused ⇒ no marker anywhere.
  for (const std::string& line : none) {
    CHECK(line.rfind("\xe2\x96\xb8", 0) != 0);
  }
  // The three labels are the same width, so the columns line up.
  CHECK(none[0].size() == none[1].size());
  CHECK(none[1].size() == none[2].size());

  const std::vector<std::string> focused =
      tui::draft_field_lines(tui::DraftField::kRef);
  std::size_t marked = 0;
  for (const std::string& line : focused) {
    if (line.rfind("\xe2\x96\xb8", 0) == 0) {
      ++marked;
      CHECK(line.find("ref") != std::string::npos);
    }
  }
  CHECK(marked == 1);
}

TEST(draft_review_shows_the_pin_the_start_command_the_tools_and_every_warning) {
  berth::DraftResult result;
  result.draft = sample_recipe();
  result.draft.tools = {"go"};
  result.draft.build_steps = {{"go", "build", "-o", "memos", "./cmd/memos"}};
  result.owner_repo = "usememos/memos";
  result.commit = result.draft.commit;
  berth::ToolStatus go;
  go.name = "go";
  go.present = true;
  go.version = "1.22";
  berth::ToolStatus bun;
  bun.name = "bun";
  bun.present = false;
  result.tools = {go, bun};
  result.warnings = {"start_cmd is an assumption — verify the app's real flags",
                     "docker-compose.yml present but ignored"};

  const std::vector<std::string> lines = tui::draft_review_lines(result);
  CHECK(contains(lines, "draft berthpanel-memos"));
  CHECK(contains(lines, "usememos/memos @ " + result.commit));
  CHECK(contains(lines, "--addr 127.0.0.1"));
  CHECK(contains(lines, "go build -o memos ./cmd/memos"));
  CHECK(contains(lines, "ok   go"));
  // A missing tool is named as missing, not as a version.
  CHECK(contains(lines, "MISS bun"));
  // Every warning verbatim: "1 warning" says nothing about WHICH.
  CHECK(contains(lines, "start_cmd is an assumption"));
  CHECK(contains(lines, "docker-compose.yml present but ignored"));
  CHECK(contains(lines, "heuristics, not truth"));
}

TEST(draft_review_says_when_there_is_nothing_to_build) {
  berth::DraftResult result;
  result.draft = sample_recipe();
  result.draft.build_steps.clear();
  result.owner_repo = "a/b";
  result.commit = result.draft.commit;
  CHECK(contains(tui::draft_review_lines(result), "a prebuilt binary"));
}

TEST(draft_clash_question_carries_both_pins) {
  // "overwrite?" is not a decision anyone can make; both pins are.
  berth::DraftClash clash;
  clash.exists = true;
  clash.service_name = "berthpanel-memos";
  clash.existing_commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  clash.existing_path =
      "/home/u/.config/berthpanel/recipes/berthpanel-memos.json";
  const std::vector<std::string> lines =
      tui::draft_clash_lines(clash, "91ab2c4f7d3e5a6b8c0d1e2f3a4b5c6d7e8f9a0b");
  CHECK(contains(lines, "overwrite berthpanel-memos?"));
  CHECK(contains(lines, "pinned 2b2192d"));
  CHECK(contains(lines, "pinned 91ab2c4"));
  CHECK(contains(lines, "stored user recipe"));
  CHECK(contains(lines, "y  yes, overwrite"));
  CHECK(contains(lines, "n  no, leave it alone"));

  berth::DraftClash curated;
  curated.exists = true;
  curated.in_catalog = true;
  curated.service_name = "berthpanel-memos";
  curated.existing_commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  CHECK(
      contains(tui::draft_clash_lines(curated, "91ab2c4"), "builtin catalog"));
}

TEST(draft_stored_says_the_draft_is_on_disk_before_it_deploys) {
  // Reproducibility, stated: what deploys is what can be read (plan 4.21).
  const std::vector<std::string> lines = tui::draft_stored_lines(
      "/home/u/.config/berthpanel/recipes/berthpanel-myapp.json");
  CHECK(contains(lines, "berthpanel-myapp.json"));
  CHECK(contains(lines, "on disk before anything deploys"));
}

TEST(draft_key_hints_follow_the_state_they_are_in) {
  // Running: cancel is the only thing on offer.
  const std::vector<std::string> running =
      tui::draft_key_hints(tui::DraftField::kNone, true, false);
  CHECK(contains(running, "c  cancel"));
  CHECK(contains(running, "nothing is stored"));
  CHECK(!contains(running, "paste a GitHub URL"));

  // A field focused: the field's own keys, and the way out.
  const std::vector<std::string> typing =
      tui::draft_key_hints(tui::DraftField::kUrl, false, false);
  CHECK(contains(typing, "Tab  next field"));
  CHECK(contains(typing, "Esc  leave the fields"));

  // A draft waiting: accept, store-only, or start over.
  const std::vector<std::string> review =
      tui::draft_key_hints(tui::DraftField::kNone, false, true);
  CHECK(contains(review, "d  store the draft and deploy it"));
  CHECK(contains(review, "s  store the draft only"));
  CHECK(contains(review, "i  paste another URL"));
}

TEST(deploy_cancellation_says_the_journal_entry_stays) {
  // §6.7: a cancelled deploy keeps its journal entry and __cleanup sweeps it.
  // A status line that made a cancellation look like a rollback would send the
  // user hunting for a half-written unit.
  const std::vector<std::string> lines =
      tui::deploy_cancelled_lines("berthpanel-memos");
  CHECK(contains(lines, "cancelled berthpanel-memos"));
  CHECK(contains(lines, "journal entry stays"));
  CHECK(contains(lines, "__cleanup"));
  CHECK(contains(lines, "nothing was deleted"));
}

TEST(deploy_hints_name_the_launch_key_and_the_cancel_key) {
  const std::vector<std::string> idle = tui::deploy_key_hints(false);
  CHECK(contains(idle, "Enter"));
  CHECK(contains(idle, "e  recipe details"));
  // While idle, 'c' would be a letter in the port field, so it must not be
  // advertised as a cancel key there.
  CHECK(!contains(idle, "cancel"));

  const std::vector<std::string> running = tui::deploy_key_hints(true);
  CHECK(contains(running, "c  cancel"));
  CHECK(contains(running, "q  quit"));
}

TEST(deploy_outcome_lines_show_the_port_the_unit_and_the_data) {
  berth::DeployOutcome outcome;
  outcome.service_name = "berthpanel-memos";
  outcome.port = 8234;
  outcome.work_dir =
      "/home/u/.local/share/berthpanel/services/berthpanel-memos";
  outcome.unit_path = "/home/u/.config/systemd/user/berthpanel-memos.service";

  const std::vector<std::string> lines = tui::deploy_outcome_lines(outcome);
  CHECK(contains(lines, "deployed berthpanel-memos"));
  CHECK(contains(lines, "port: 8234"));
  CHECK(contains(lines, "berthpanel-memos.service"));
  // Uninstall keeps the data dir on purpose; the screen must not imply the
  // deploy owned it (§12.12).
  CHECK(contains(lines, "leaves the data dir in place"));
}

TEST(deploy_error_lines_keep_the_whole_error_text) {
  // §11: the full error, not a summary. deploy.cpp adds context at every
  // boundary and the screen must not truncate it away.
  const std::vector<std::string> lines = tui::deploy_error_lines(
      "deploy: build failed: exit 1\n  (journal entry kept for cleanup)");
  CHECK(lines.size() == 3);
  CHECK(lines[0] == "deploy failed:");
  CHECK(contains(lines, "journal entry kept for cleanup"));
  CHECK(tui::deploy_error_lines("").empty());
}

TEST(deploy_idle_lines_say_which_port_will_be_used) {
  berth::Recipe recipe;
  recipe.service_name = "berthpanel-memos";
  recipe.owner = "usememos";
  recipe.name = "memos";
  recipe.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";

  CHECK(contains(tui::deploy_idle_lines(recipe, 0, false), "port: free"));
  CHECK(contains(tui::deploy_idle_lines(recipe, 9090, true), "port: 9090"));

  recipe.port = 8312;
  const std::vector<std::string> defaulted =
      tui::deploy_idle_lines(recipe, 0, false);
  CHECK(contains(defaulted, "port: 8312"));
  CHECK(contains(defaulted, "the recipe's own default"));
}

TEST(deploy_tail_integration_stays_bounded_and_oldest_first) {
  // The screen's buffer is a ring (dev.md §5: never a vector drained from the
  // front). This is the integration the deploy screen depends on: a long build
  // streams thousands of lines and the pane must neither grow nor reorder.
  // `text` keeps the stamp, because on the tail pane the time is information.
  tui::RingBuffer<std::string> tail(8);
  for (int i = 0; i < 40; ++i) {
    tail.push_back(
        tui::parse_deploy_log_line(
            stamped("build berthpanel-memos line " + std::to_string(i)))
            .text);
  }
  CHECK(tail.size() == 8);
  CHECK(tail.dropped() == 32);
  CHECK(tail.at(0).find("line 32") != std::string::npos);
  CHECK(tail.at(7).find("line 39") != std::string::npos);

  // Every retained line parses back to the build stage: the reader and the
  // parser agree on what the engine writes.
  for (std::size_t i = 0; i < tail.size(); ++i) {
    CHECK(tui::parse_deploy_stage(tail.at(i)) == tui::DeployStage::kBuild);
  }
}