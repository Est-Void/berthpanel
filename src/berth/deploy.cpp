#include "deploy.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../analyzer/ports.h"
#include "../core/deploylog.h"
#include "../core/names.h"
#include "../core/netlog.h"
#include "../core/state.h"
#include "../util/fs.h"
#include "../util/sleep.h"
#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/which.h"
#include "cancel.h"
#include "catalog.h"
#include "gitclone.h"
#include "journal.h"
#include "lock.h"
#include "port.h"
#include "prefetch.h"
#include "preflight.h"
#include "sandbox.h"
#include "secrets.h"
#include "toolbox.h"
#include "units.h"
#include "util/fs.h"

namespace berth {

namespace {

RecipeLike recipe_view(const Recipe& recipe) {
  RecipeLike view;
  view.service_name = recipe.service_name;
  view.owner_repo = recipe.owner_repo();
  view.commit = recipe.commit;
  view.language = recipe.language;
  view.start_cmd = recipe.start_cmd;
  view.tools = recipe.tools;
  view.build_steps = recipe.build_steps;
  view.port = recipe.port;
  view.has_memory_limit = !recipe.res.memory_max.empty();
  view.has_tasks_limit = recipe.res.tasks_max != 0;
  return view;
}

}  // namespace

Result<std::vector<std::string>> resolve_start_argv(
    const Recipe& recipe, const std::filesystem::path& work_dir,
    const std::uint16_t port, std::string& binary) {
  const Result<std::vector<std::string>> tokens =
      split_command_template(recipe.start_cmd);
  if (tokens.is_err())
    return std::move(tokens).error();
  const std::string self = (work_dir / recipe.binary_name).string();
  std::vector<std::pair<std::string, std::string>> values;
  values.emplace_back("bin", self);
  values.emplace_back("project", work_dir.string());
  values.emplace_back("port", std::to_string(port));
  values.emplace_back("self", self);
  std::vector<std::string> resolved;
  for (const std::string& token : tokens.value()) {
    const Result<std::string> substituted =
        substitute_placeholders(token, values);
    if (substituted.is_err())
      return std::move(substituted).error();
    resolved.push_back(std::move(substituted).value());
  }
  // A non-absolute program (a bare `bun`, or the `env` Gitea uses) must be
  // resolved HERE: systemd's PATH is not the panel's, so a bare name passes
  // preflight and the unit then dies at start. {self}/{bin} are absolute and
  // skip this.
  std::string& program = resolved.front();
  if (program.empty() || program.front() != '/') {
    const Result<std::string> absolute = resolve_executable(program);
    if (absolute.is_err()) {
      return with_context(
          std::move(absolute).error(),
          "deploy: resolving start interpreter '" + program + "'");
    }
    program = absolute.value();
  }
  binary = program;
  resolved.erase(resolved.begin());
  return resolved;
}

Result<void> RealDeployOps::fetch_source(const Recipe& recipe,
                                         const std::filesystem::path& work_dir,
                                         const Paths& paths,
                                         const DeployOptions& options) {
  Result<void> dir = ensure_dir(work_dir);
  if (dir.is_err())
    return std::move(dir).error();
  if (recipe.prebuilt.has_value()) {
    const PinnedTool tool{.name = recipe.service_name,
                          .url = recipe.prebuilt->url,
                          .sha256 = recipe.prebuilt->sha256,
                          .inner_path = recipe.prebuilt->member,
                          .install_dir = work_dir,
                          .executable = true,
                          .archive = recipe.prebuilt->archive};
    const Result<std::filesystem::path> installed =
        install_pinned_tool(tool, paths, options.cancel);
    if (installed.is_err())
      return std::move(installed).error();
    return {};
  }
  // §6.1: a git clone is an outbound request to github.com that does not
  // run through httpclient, so the attempt is logged here — BEFORE it
  // runs, so the line exists even if the process dies mid-clone.
  const std::string note = "clone " + recipe.owner_repo() + "@" + recipe.commit;
  const Result<void> logged =
      netlog_append(paths, NetlogEntry{.host = kTokenHost,
                                       .ip = "",
                                       .port = 443,
                                       .purpose = "git-fetch",
                                       .note = note,
                                       .allowed = true});
  if (logged.is_err())
    return logged.error();
  // A pinned clone cannot land on top of an existing checkout, and the
  // refusal that follows ("destination exists and is not empty") says nothing
  // about WHY there is something there — so the two cases are told apart here,
  // before the clone runs and while we still know what the service is.
  const Result<std::optional<ServiceRecord>> registered =
      registry_get(paths, recipe.service_name);
  if (registered.is_err()) {
    return with_context(std::move(registered).error(),
                        "deploy: checking whether " + recipe.service_name +
                            " is already deployed");
  }
  std::error_code ec;
  const bool occupied = std::filesystem::exists(work_dir, ec) &&
                        !std::filesystem::is_empty(work_dir, ec);
  if (occupied) {
    if (registered.value().has_value()) {
      // A LIVE service's checkout: replacing the tree under a running service
      // is not a repair, it is a surprise. Refuse, and say what to do instead.
      return Error(
          "deploy: the checkout for " + recipe.service_name +
          " is already in " + work_dir.string() +
          " and a pinned clone will not overwrite it. That service IS "
          "registered, so this is a repair rather than a first deploy: its "
          "unit "
          "is already serving. To rebuild it, move the work dir aside yourself "
          "(it holds your data) — berthpanel will not delete the tree a "
          "registered service runs from.");
    }
    // NOT registered ⇒ leftover from an interrupted deploy, which §6.7 already
    // treats as sweepable. Remove it here, and SAY so in deploy.log: a silent
    // rm of a work dir is exactly what this project refuses to do.
    const Result<std::uintmax_t> removed = remove_tree(work_dir);
    if (removed.is_err()) {
      return with_context(std::move(removed).error(),
                          "deploy: clearing " + work_dir.string() +
                              " (leftover from an interrupted deploy)");
    }
    const Result<void> noted = deploylog_append(
        paths, "deploy: cleared " + work_dir.string() + " (" +
                   std::to_string(removed.value()) +
                   " entries) left by an interrupted deploy of " +
                   recipe.service_name + " — it was not registered");
    static_cast<void>(noted);
    const Result<void> ready = ensure_dir(work_dir);
    if (ready.is_err()) {
      return std::move(ready).error();
    }
  }
  return clone_pinned_github(recipe.owner_repo(), recipe.commit, work_dir,
                             options.token, std::chrono::milliseconds(600000),
                             options.cancel);
}

Result<void> RealDeployOps::prefetch(const Recipe& recipe,
                                     const std::filesystem::path& work_dir,
                                     const Paths& paths,
                                     const DeployOptions& options) {
  static_cast<void>(paths);
  if (recipe.prefetch_steps.empty())
    return {};
  Result<std::size_t> ran =
      run_prefetch(recipe.prefetch_steps, work_dir, options.cancel);
  if (ran.is_err())
    return std::move(ran).error();
  return {};
}

Result<void> RealDeployOps::build(const Recipe& recipe,
                                  const std::filesystem::path& work_dir,
                                  const std::uint16_t port, const Paths& paths,
                                  const DeployOptions& options) {
  static_cast<void>(paths);
  Result<std::string> build_stem = build_unit_name(recipe.service_name);
  if (build_stem.is_err())
    return std::move(build_stem).error();
  for (const std::vector<std::string>& step : recipe.build_steps) {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> values;
    values.emplace_back("project", work_dir.string());
    values.emplace_back("port", std::to_string(port));
    values.emplace_back("bin", work_dir.string());
    values.emplace_back("self", work_dir.string());
    for (const std::string& element : step) {
      const Result<std::string> substituted =
          substitute_placeholders(element, values);
      if (substituted.is_err())
        return std::move(substituted).error();
      argv.push_back(std::move(substituted).value());
    }
    const SandboxSpec spec{.argv = argv,
                           .working_dir = work_dir,
                           .env_extra = {},
                           .unit_name = build_stem.value(),
                           .memory_max_mb = 2048,
                           .tasks_max = 512,
                           .timeout = std::chrono::milliseconds(1800000),
                           .cancel = options.cancel};
    const Result<int> exit_code = run_in_sandbox(spec);
    if (exit_code.is_err())
      return std::move(exit_code).error();
    if (exit_code.value() != 0) {
      return Error("deploy: build step '" + argv.front() +
                   "' failed with exit " + std::to_string(exit_code.value()));
    }
  }
  return {};
}

Result<std::filesystem::path> RealDeployOps::write_unit_file(
    const Recipe& recipe, const std::filesystem::path& work_dir,
    const std::uint16_t port, const Paths& paths) {
  std::string binary;
  const Result<std::vector<std::string>> args =
      resolve_start_argv(recipe, work_dir, port, binary);
  if (args.is_err())
    return std::move(args).error();

  UnitSpec spec;
  spec.service_name = recipe.service_name;
  spec.description = recipe.display_name;
  spec.working_dir = work_dir;
  spec.secrets_file = paths.secrets_dir / (recipe.service_name + ".env");
  spec.exec_binary = binary;
  spec.exec_args = args.value();
  spec.limits = UnitLimits{.memory_max = recipe.res.memory_max,
                           .tasks_max = recipe.res.tasks_max};
  spec.read_write_paths = {work_dir};
  if (!std::filesystem::exists(spec.secrets_file)) {
    Result<std::filesystem::path> written =
        write_secrets(paths, recipe.service_name, {});
    if (written.is_err())
      return std::move(written).error();
  }
  return write_unit(paths, spec);
}

Result<void> RealDeployOps::start_service(const std::string& service_name) {
  return systemctl_enable_now(service_name);
}

namespace {

// Errors raised inside this file already start with "deploy: ", and the call
// sites below add it again to say which step failed. "deploy: deploy: ..." in
// the panel's error line is noise on a message the user has to act on.
std::string step_error(const std::string& step, std::string ctx) {
  if (ctx.rfind("deploy: ", 0) == 0) {
    return ctx;
  }
  return "deploy: " + step + ": " + std::move(ctx);
}

}  // namespace

std::string parse_systemctl_show(const std::string_view show) {
  // The listed properties, in a fixed output order: systemctl prints its own
  // order, which is not what we asked for, so we must not depend on it.
  constexpr std::array<std::string_view, 5> kKeys{
      "Result", "ExecMainStatus", "ExecMainCode", "ActiveState", "SubState"};
  std::array<std::string, kKeys.size()> values;
  std::array<bool, kKeys.size()> found{};
  for (const std::string& raw : split(show, "\n")) {
    const std::string_view line(raw);
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = line.substr(0, eq);
    for (std::size_t i = 0; i < kKeys.size(); ++i) {
      if (key == kKeys[i]) {
        values[i] = std::string(trim(line.substr(eq + 1)));
        found[i] = true;
      }
    }
  }
  std::string out;
  for (std::size_t i = 0; i < kKeys.size(); ++i) {
    if (!found[i]) {
      continue;
    }
    if (!out.empty()) {
      out += ' ';
    }
    out += kKeys[i];
    out += '=';
    out += values[i];
  }
  return out;
}

std::string bound_journal_tail(const std::string_view text,
                               const std::size_t max_lines,
                               const std::size_t max_bytes) {
  if (max_lines == 0 || max_bytes == 0) {
    return {};
  }
  // Trailing blank lines are noise, not lines to show; dropping them first
  // makes the line count match what a reader would call the tail.
  std::size_t end = text.size();
  while (end > 0 && text[end - 1] == '\n') {
    --end;
  }
  const std::string_view body = text.substr(0, end);
  std::size_t start = 0;
  std::size_t seen = 0;
  for (std::size_t i = body.size(); i > 0; --i) {
    if (body[i - 1] == '\n') {
      ++seen;
      if (seen == max_lines) {
        start = i;
        break;
      }
    }
  }
  std::string_view tail = body.substr(start);
  if (tail.size() > max_bytes) {
    tail = tail.substr(tail.size() - max_bytes);
    // The byte cap can land mid-line; drop the partial leading line so the
    // evidence never starts with a truncated word. A single over-long line
    // has no newline to cut on and is kept as the capped slice.
    const std::size_t nl = tail.find('\n');
    if (nl != std::string_view::npos) {
      tail = tail.substr(nl + 1);
    }
  }
  return std::string(tail);
}

namespace {

constexpr std::size_t kFailureEvidenceMaxLines = 20;
constexpr std::size_t kFailureEvidenceMaxBytes = 4096;

// Gathers the unit's OWN evidence for a failed start: the systemd verdict and
// the bounded journal tail, each line prefixed so the panel's log pane reads
// as the app's words. Cannot fail: a missing systemctl/journalctl, a timeout
// or a cancellation are handled by skipping that part, so a start failure
// never turns into a different failure. Never runs on the UI thread.
[[nodiscard]] std::string capture_unit_failure_evidence(
    const std::string& service_name, std::atomic<bool>* cancel) {
  std::string evidence;
  const Result<std::string> systemctl = resolve_executable("systemctl");
  if (systemctl.is_ok()) {
    RunLimits limits;
    limits.timeout = std::chrono::milliseconds(5000);
    limits.cancel = cancel;
    const Result<RunOutput> out =
        run({systemctl.value(), "--user", "show", service_name + ".service",
             "-p", "Result", "-p", "ExecMainStatus", "-p", "ExecMainCode", "-p",
             "ActiveState", "-p", "SubState"},
            limits);
    if (out.is_ok()) {
      const std::string verdict = parse_systemctl_show(out.value().out);
      if (!verdict.empty()) {
        evidence += "systemd: " + verdict;
      }
    }
  }
  const Result<std::string> journalctl = resolve_executable("journalctl");
  if (journalctl.is_ok()) {
    RunLimits limits;
    limits.timeout = std::chrono::milliseconds(5000);
    limits.cancel = cancel;
    const Result<RunOutput> out =
        run({journalctl.value(), "--user", "-u", service_name + ".service",
             "-n", "20", "--no-pager", "--output=cat"},
            limits);
    if (out.is_ok()) {
      const std::string tail = bound_journal_tail(
          out.value().out, kFailureEvidenceMaxLines, kFailureEvidenceMaxBytes);
      for (const std::string& line : split(tail, "\n")) {
        if (line.empty()) {
          continue;
        }
        if (!evidence.empty()) {
          evidence += '\n';
        }
        evidence += "service output: " + line;
      }
    }
  }
  return evidence;
}

}  // namespace

Result<void> RealDeployOps::await_running(const std::string& service_name,
                                          const Paths& paths,
                                          std::atomic<bool>* cancel) {
  Result<std::string> active = wait_for_active_unit(
      service_name, std::chrono::milliseconds(8000), cancel);
  if (active.is_err()) {
    return std::move(active).error();
  }
  // "active" only means the process was SPAWNED (Type=simple). A service whose
  // start_cmd is wrong passes that check and dies a second later, looping on
  // Restart=on-failure — and the deploy would have reported success. So it has
  // to STAY active to be called a deploy.
  const Result<std::string> systemctl = resolve_executable("systemctl");
  if (systemctl.is_err()) {
    return std::move(systemctl).error();
  }
  std::vector<std::string> observed;
  const auto settle_until = std::chrono::steady_clock::now() + kSettleWindow;
  while (std::chrono::steady_clock::now() < settle_until) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error("deploy: cancelled while waiting for " + service_name);
    }
    RunLimits limits;
    limits.timeout = std::chrono::milliseconds(5000);
    Result<RunOutput> out = run(
        {systemctl.value(), "--user", "is-active", service_name + ".service"},
        limits);
    std::string state = "unknown";
    if (out.is_ok()) {
      state = berth::trim(out.value().out);
      if (state.empty()) {
        state = "unknown";
      }
    } else {
      state = out.error().ctx();
    }
    observed.push_back(state);
    if (state != "active") {
      // No point waiting out the rest of the window on a unit that already
      // left: the verdict is already decided.
      break;
    }
    interruptible_sleep(std::chrono::milliseconds(250));
  }
  Result<std::string> settled = settle_active(service_name, observed);
  if (settled.is_err()) {
    // The unit is down: surface ITS OWN words, in the error and in deploy.log
    // (the panel reads the log), instead of sending the user to journalctl.
    const std::string evidence =
        capture_unit_failure_evidence(service_name, cancel);
    if (!evidence.empty()) {
      for (const std::string& line : split(evidence, "\n")) {
        const Result<void> logged = deploylog_append(paths, line);
        static_cast<void>(logged);
      }
      settled = settle_active(service_name, observed, evidence);
    }
    return std::move(settled).error();
  }
  return {};
}

Result<void> RealDeployOps::verify_listening(const std::string& service_name,
                                             const std::uint16_t port,
                                             const Paths& paths,
                                             std::atomic<bool>* cancel) {
  const auto deadline = std::chrono::steady_clock::now() + kListenWindow;
  while (true) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error("deploy: cancelled while checking that " + service_name +
                   " is listening on port " + std::to_string(port));
    }
    // /proc/net only — the same dumb, unattributed reader System Scan uses
    // (§7): no /proc/<pid> walk, no socket-inode guess at who owns the port.
    Result<PortReport> report = listening_ports(paths);
    if (report.is_err()) {
      // A probe that cannot run is not evidence of absence (§11). Warn loudly
      // so the failure mode is visible, then let an otherwise-active deploy
      // proceed rather than fail on the checker itself.
      const Result<void> warned = deploylog_append(
          paths, "WARNING: could not verify that " + service_name +
                     " is listening on port " + std::to_string(port) + " (" +
                     report.error().ctx() +
                     ") — deploy continues without the check");
      static_cast<void>(warned);
      return {};
    }
    Result<void> verdict =
        verify_loopback_listener(report.value(), port, service_name);
    if (verdict.is_ok()) {
      return {};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return verdict;  // the last successful probe's verdict, loud
    }
    interruptible_sleep(std::chrono::milliseconds(250));
  }
}

Result<std::string> wait_for_active_unit(const std::string& service_name,
                                         const std::chrono::milliseconds budget,
                                         std::atomic<bool>* cancel) {
  if (!validate_unit_name(service_name)) {
    return Error("deploy: invalid service name: '" + service_name + "'");
  }
  Result<std::string> binary = resolve_executable("systemctl");
  if (binary.is_err())
    return std::move(binary).error();
  const auto deadline = std::chrono::steady_clock::now() + budget;
  std::string last = "no answer";
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error("deploy: cancelled while waiting for " + service_name);
    }
    RunLimits limits;
    limits.timeout = std::chrono::milliseconds(5000);
    Result<RunOutput> out =
        run({binary.value(), "--user", "is-active", service_name + ".service"},
            limits);
    if (out.is_ok()) {
      std::string state = out.value().out;
      while (!state.empty() && (state.back() == '\n' || state.back() == ' ')) {
        state.pop_back();
      }
      if (state == "active")
        return state;
      last = state.empty() ? "unknown" : state;
    } else {
      last = out.error().ctx();
    }
    interruptible_sleep(std::chrono::milliseconds(250));
  }
  return Error("deploy: " + service_name + " did not reach 'active' within " +
               std::to_string(budget.count()) + "ms (last state: " + last +
               ")");
}

Result<std::string> settle_active(const std::string& service_name,
                                  const std::vector<std::string>& states,
                                  const std::string& evidence) {
  if (states.empty()) {
    // Vacuous truth is not evidence: a settle window that never got an answer
    // has confirmed nothing, and must not report a service as up.
    std::string message = "deploy: " + service_name +
                          " was never confirmed active after starting — no "
                          "answer from systemctl during the settle window";
    if (!evidence.empty()) {
      message += "\n" + evidence;
    }
    return Error(std::move(message));
  }
  for (const std::string& state : states) {
    if (state != "active") {
      // The most common cause by far is a start_cmd the app does not
      // understand — the detector's is an ASSUMPTION and says so — and the
      // app's OWN verdict now rides along, so the user is not sent to the
      // unit logs for a flag typo.
      std::string message = "deploy: ";
      message += service_name;
      message += " came up and then left 'active' (state: ";
      message += state;
      message +=
          ") — it is not serving. the start_cmd is wrong; fix it with "
          "'berthpanel __recipe-edit " +
          service_name + " --start-cmd ...' or 'E' in the panel.";
      if (!evidence.empty()) {
        message += "\n" + evidence;
      }
      return Error(std::move(message));
    }
  }
  return std::string("active");
}

namespace {

// The observed listeners as a bounded "address:port" list. Bounded because a
// busy host would otherwise push the actual diagnosis out of the error line.
std::string render_observed_listeners(const std::vector<ListeningPort>& ports) {
  if (ports.empty()) {
    return "(none)";
  }
  constexpr std::size_t kMaxListed = 12;
  std::string out;
  for (std::size_t i = 0; i < ports.size() && i < kMaxListed; ++i) {
    if (!out.empty()) {
      out += ", ";
    }
    out += ports[i].address;
    out += ':';
    out += std::to_string(ports[i].port);
  }
  if (ports.size() > kMaxListed) {
    out += ", ... (+" + std::to_string(ports.size() - kMaxListed) + " more)";
  }
  return out;
}

}  // namespace

Result<void> verify_loopback_listener(const PortReport& report,
                                      const std::uint16_t expected_port,
                                      const std::string& service_name) {
  bool listening_on_port = false;
  bool wildcard_only = true;
  for (const ListeningPort& port : report.ports) {
    if (port.port != expected_port) {
      continue;
    }
    if (port.loopback) {
      return {};  // a loopback listener on the recorded port is the success
    }
    listening_on_port = true;
    if (!port.wildcard) {
      wildcard_only = false;  // a specific non-loopback bind, not the wildcard
    }
  }
  const std::string observed = render_observed_listeners(report.ports);
  if (!listening_on_port) {
    // The §11 silent wrong answer: the unit is active, so `enable --now` and
    // the settle window both said success, but the port the registry is about
    // to record belongs to nobody. The fixed-port recipe whose app hardcodes a
    // different port is the concrete case; name it.
    return Error(
        "deploy: " + service_name +
        " is active but nothing is listening on port " +
        std::to_string(expected_port) +
        " (the port this deploy recorded), so it is not serving. observed "
        "listeners: " +
        observed +
        ". a fixed-port recipe whose app binds a different port must set "
        "`port` to match, or start_cmd must pass the app's real port "
        "flag/placeholder ({port}); fix it with 'berthpanel __recipe-edit " +
        service_name + " --start-cmd ...' or 'E' in the panel.");
  }
  // A listener on the port exists but none of it is loopback: binding beyond
  // 127.0.0.1/::1 is the exposure the loopback-only invariant forbids.
  const std::string where = wildcard_only
                                ? "only on a wildcard address (0.0.0.0/::)"
                                : "on a non-loopback address";
  return Error(
      "deploy: " + service_name + " is listening on port " +
      std::to_string(expected_port) + " but " + where +
      ", so it is exposed beyond the host — that is the exposure the "
      "loopback-only invariant forbids. observed listeners: " +
      observed +
      ". bind the app to 127.0.0.1 (the recipe's bind or the app's host flag) "
      "and redeploy.");
}

Result<void> RealDeployOps::register_service(const ServiceRecord& record,
                                             const Paths& paths) {
  return registry_register(paths, record);
}

Result<bool> RealDeployOps::stop_unit(const std::string& unit_stem,
                                      std::atomic<bool>* cancel) {
  SystemdUnitTerminator terminator;
  return terminator.stop(unit_stem, cancel);
}

namespace {

bool work_dir_matches_layout(const Paths& paths, const JournalEntry& entry) {
  const std::filesystem::path expected =
      (paths.services_dir / entry.service_name).lexically_normal();
  return std::filesystem::path(entry.work_dir).lexically_normal() == expected;
}

// True unless entry.work_dir, canonically resolved, lands STRICTLY inside
// paths.services_dir. Compared component by component — never as a string
// prefix, or "/srv/x" would count as inside "/srv/xy". weakly_canonical
// resolves the existing path, so a symlinked component cannot fake
// containment. A filesystem error counts as outside: the sweep then
// refuses to act on the entry instead of guessing.
bool work_dir_outside_services(const Paths& paths, const JournalEntry& entry) {
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::weakly_canonical(paths.services_dir, ec);
  if (ec)
    return true;
  const std::filesystem::path work =
      std::filesystem::weakly_canonical(entry.work_dir, ec);
  if (ec)
    return true;

  auto dir_it = dir.begin();
  const auto dir_end = dir.end();
  auto work_it = work.begin();
  const auto work_end = work.end();
  for (; dir_it != dir_end; ++dir_it, ++work_it) {
    if (work_it == work_end || *work_it != *dir_it)
      return true;
  }
  return work_it == work_end;  // equal ⇒ not strictly inside
}

void log_sweep(const Paths& paths, const std::string& line) {
  const Result<void> logged = deploylog_append(paths, line);
  static_cast<void>(logged);
}

// Removes a unit artifact — the unit file or the `enable` symlink under
// default.target.wants/. Returns true when something was removed. remove(2)
// does not follow a symlink (so a DANGLING wants link is removed as a link)
// and does not report an error for a missing path, which is exactly what we
// need; symlink_status/exists would either follow the link or raise ENOENT
// when the parent directory is absent.
Result<bool> remove_unit_artifact(const std::filesystem::path& path) {
  std::error_code ec;
  const bool removed = std::filesystem::remove(path, ec);
  if (ec)
    return Error("remove " + path.string() + ": " + ec.message());
  return removed;
}

}  // namespace

Result<ReconcileReport> sweep_interrupted_entries(
    const Paths& paths, DeployOps& ops,
    const std::vector<JournalEntry>& entries, std::atomic<bool>* cancel) {
  // PRECONDITION (deploy.h): the deploy lock is held or proven free. Under
  // that lock, "journal entry present" IS an interrupted deploy (§6.7) —
  // unless the service is in the registry, which means the deploy completed
  // and only the journal write-back was lost. The registry is read once,
  // up front: if it cannot be read, no entry can be proven unregistered and
  // nothing may be deleted.
  ReconcileReport report;
  Result<std::vector<ServiceRecord>> registry = registry_list(paths);
  if (registry.is_err())
    return std::move(registry).error();
  const auto in_registry = [&](const std::string& service_name) {
    for (const ServiceRecord& record : registry.value()) {
      if (record.service_name == service_name)
        return true;
    }
    return false;
  };
  for (const JournalEntry& entry : entries) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      report.failed.emplace_back(entry.service_name, "sweep cancelled");
      continue;
    }
    const auto fail = [&](const std::string& why) {
      report.failed.emplace_back(entry.service_name, why);
    };

    if (!validate_unit_name(entry.service_name)) {
      fail("invalid service name in journal");
      continue;
    }

    // Path guard, before anything is stopped or removed: a work_dir that
    // does not resolve strictly inside THIS services_dir is never swept —
    // it may belong to a completely different install (swapped XDG env,
    // hand-edited journal, symlink escape). Loud and permanent.
    if (work_dir_outside_services(paths, entry)) {
      log_sweep(paths, "cleanup: " + entry.service_name + " work_dir '" +
                           entry.work_dir + "' is outside " +
                           paths.services_dir.string() + " — nothing deleted");
      report.skipped_outside.push_back(entry.service_name);
      continue;
    }

    if (!work_dir_matches_layout(paths, entry)) {
      // §11: validate every value that reaches a path. A hand-edited
      // journal entry never steers a delete outside the services layout.
      fail("journal work_dir '" + entry.work_dir + "' is not " +
           (paths.services_dir / entry.service_name).string());
      continue;
    }

    // Registry guard: a registered service was fully deployed — its
    // journal entry is stale, its files are NOT cleanup's to remove.
    if (in_registry(entry.service_name)) {
      log_sweep(paths, "cleanup: " + entry.service_name +
                           " is in the registry — stale journal entry "
                           "dropped, nothing deleted");
      Result<void> cleared = journal_remove(paths, entry.service_name);
      if (cleared.is_err()) {
        fail(cleared.error().ctx());
        continue;
      }
      report.skipped_registry.push_back(entry.service_name);
      continue;
    }

    bool ok = true;
    std::string why;

    // 1. The app unit may be LIVE: a deploy that died at 'registering'
    //    started it before the registry write was confirmed. Stop it, or
    //    the work-dir removal races a running service.
    Result<bool> stopped = ops.stop_unit(entry.service_name, cancel);
    if (stopped.is_err()) {
      ok = false;
      why = stopped.error().ctx();
    } else if (stopped.value()) {
      log_sweep(paths, "cleanup stopped unit " + entry.service_name);
    }

    // 2. The orphaned build unit (systemd-run --wait keeps the transient
    //    unit running when berthpanel dies mid-build). Only a deploy that
    //    got past build_unit_name() can have one — the same function
    //    decides, so an invalid name here proves no unit exists.
    if (ok) {
      Result<std::string> build_stem = build_unit_name(entry.service_name);
      if (build_stem.is_err()) {
        log_sweep(paths, "cleanup: no stoppable build unit for " +
                             entry.service_name + ": " +
                             build_stem.error().ctx());
      } else {
        Result<bool> build_stopped = ops.stop_unit(build_stem.value(), cancel);
        if (build_stopped.is_err()) {
          ok = false;
          why = build_stopped.error().ctx();
        } else if (build_stopped.value()) {
          log_sweep(paths, "cleanup stopped orphaned build unit " +
                               build_stem.value());
        }
      }
    }

    // 3. The work dir.
    if (ok) {
      Result<std::size_t> removed = remove_tree(entry.work_dir);
      if (removed.is_err()) {
        ok = false;
        why = removed.error().ctx();
      } else {
        log_sweep(paths, "cleanup removed " + entry.work_dir + " (" +
                             std::to_string(removed.value()) + " entries)");
      }
    }

    // 4. The unit file, if the deploy died at 'registering' with the unit
    //    already written — plus the `enable` symlink start_service created
    //    under default.target.wants/. Removing only the unit file leaves a
    //    dangling wants symlink behind, the exact leftover this sweep
    //    exists to prevent. No daemon-reload: a loaded-but-fileless unit is
    //    inert, and the next enable does a reload anyway.
    if (ok) {
      const std::filesystem::path unit_file =
          paths.units_dir / (entry.service_name + ".service");
      Result<bool> removed_unit = remove_unit_artifact(unit_file);
      if (removed_unit.is_err()) {
        ok = false;
        why = removed_unit.error().ctx();
      } else if (removed_unit.value()) {
        log_sweep(paths, "cleanup removed unit file " + unit_file.string());
      }
    }
    if (ok) {
      const std::filesystem::path wants_link =
          paths.units_dir / "default.target.wants" /
          (entry.service_name + ".service");
      Result<bool> removed_link = remove_unit_artifact(wants_link);
      if (removed_link.is_err()) {
        ok = false;
        why = removed_link.error().ctx();
      } else if (removed_link.value()) {
        log_sweep(paths,
                  "cleanup removed enable symlink " + wants_link.string());
      }
    }

    // Secrets deliberately stay: the file may hold user-provided
    // environment values (stage 4B), it is rewritten on the next deploy
    // of the same recipe, and cleanup must not delete user data.

    // 5. The journal entry goes LAST: "still present" is exactly what a
    //    half-finished sweep relies on for its next retry.
    if (!ok) {
      fail(why);
      continue;
    }
    Result<void> cleared = journal_remove(paths, entry.service_name);
    if (cleared.is_err()) {
      fail(cleared.error().ctx());
      continue;
    }
    report.swept.push_back(ReconcileReport::Swept{
        .service_name = entry.service_name, .stage = entry.stage});
    log_sweep(paths, "cleanup reconciled " + entry.service_name +
                         " (interrupted at '" + entry.stage + "')");
  }
  return report;
}

Result<ReconcileReport> reconcile_interrupted(const Paths& paths,
                                              DeployOps& ops,
                                              std::atomic<bool>* cancel) {
  Result<std::vector<JournalEntry>> entries = journal_load(paths);
  if (entries.is_err())
    return std::move(entries).error();
  ReconcileReport report;
  if (entries.value().empty())
    return report;

  // The lock decides liveness, not timestamps (§6.7): a sweep must never
  // race a live deploy, and taking the lock is what makes "entry present"
  // mean "interrupted".
  Result<DeployLock> lock = DeployLock::acquire(paths);
  if (lock.is_err()) {
    if (lock.error().ctx().find("already running") != std::string::npos) {
      report.lock_busy = true;
      return report;
    }
    return std::move(lock).error();
  }
  return sweep_interrupted_entries(paths, ops, entries.value(), cancel);
}

std::string render_reconcile_report(const ReconcileReport& report) {
  std::string out;
  if (report.lock_busy) {
    out += "a deploy is running right now; nothing was swept\n";
    return out;
  }
  if (report.swept.empty() && report.failed.empty() &&
      report.skipped_registry.empty() && report.skipped_outside.empty()) {
    out += "nothing to clean up\n";
    return out;
  }
  for (const ReconcileReport::Swept& swept : report.swept) {
    out += "reconciled " + swept.service_name + " (interrupted at '" +
           swept.stage + "')\n";
  }
  for (const std::string& skipped : report.skipped_registry) {
    out += "SKIPPED " + skipped +
           ": in the registry — stale journal entry dropped, nothing deleted\n";
  }
  for (const std::string& skipped : report.skipped_outside) {
    out += "SKIPPED " + skipped +
           ": journal work_dir is outside the services layout — entry kept, "
           "nothing deleted\n";
  }
  for (const std::pair<std::string, std::string>& failure : report.failed) {
    out += "FAILED " + failure.first + ": " + failure.second + "\n";
  }
  return out;
}

Result<DeployOutcome> run_deploy(const Paths& paths, const Recipe& recipe,
                                 const std::uint16_t forced_port,
                                 DeployOps& ops, const DeployOptions& options) {
  const Result<void> analysed = validate_recipe(recipe);
  if (analysed.is_err())
    return std::move(analysed).error();

  Result<DeployLock> lock = DeployLock::acquire(paths);
  if (lock.is_err())
    return std::move(lock).error();

  // Reconciliation (§6.7): we hold the exclusive lock, so any journal
  // entry still present belongs to an interrupted deploy. Sweep it before
  // any new work — a deploy never proceeds on top of interrupted state,
  // and a rerun after "unit live but not registered" starts clean.
  Result<std::vector<JournalEntry>> stale = journal_load(paths);
  if (stale.is_err())
    return std::move(stale).error();
  Result<ReconcileReport> swept =
      sweep_interrupted_entries(paths, ops, stale.value(), options.cancel);
  if (swept.is_err())
    return std::move(swept).error();
  for (const std::pair<std::string, std::string>& failure :
       swept.value().failed) {
    static_cast<void>(deploylog_append(
        paths, "cleanup failed for " + failure.first + ": " + failure.second));
    if (failure.first == recipe.service_name) {
      return Error("deploy: interrupted deploy for " + failure.first +
                   " could not be cleaned up, refusing to build on top of "
                   "it: " +
                   failure.second);
    }
  }

  const auto log = [&paths](const std::string& line) {
    const Result<void> logged = deploylog_append(paths, line);
    static_cast<void>(logged);
  };

  Result<std::vector<ToolStatus>> tools =
      check_tools(recipe.tools, options.cancel);
  if (tools.is_err())
    return std::move(tools).error();
  const Result<PreflightReport> preflight =
      preflight_recipe(recipe_view(recipe), tools.value());
  if (preflight.is_err())
    return std::move(preflight).error();
  if (!preflight.value().ready()) {
    return Error("deploy: preflight blocked this recipe:\n" +
                 render_preflight(preflight.value()));
  }

  std::uint16_t port = recipe.port;
  if (forced_port != 0) {
    port = forced_port;
  } else if (port == 0) {
    Result<std::vector<ServiceRecord>> registered = registry_list(paths);
    if (registered.is_err())
      return std::move(registered).error();
    std::vector<std::uint16_t> taken;
    for (const ServiceRecord& record : registered.value()) {
      if (record.port != 0)
        taken.push_back(record.port);
    }
    Result<std::uint16_t> picked = pick_free_port(taken);
    if (picked.is_err())
      return std::move(picked).error();
    port = picked.value();
  }

  const std::filesystem::path work_dir =
      paths.services_dir / recipe.service_name;
  const auto stage = [&](const char* name) -> Result<void> {
    JournalEntry entry;
    entry.service_name = recipe.service_name;
    entry.work_dir = work_dir.string();
    entry.repo = recipe.owner_repo();
    entry.commit = recipe.commit;
    entry.stage = name;
    entry.started_at_unix = static_cast<std::int64_t>(::time(nullptr));
    // NOT discarded: the journal entry is the record §6.7 cleanup finds an
    // interrupted deploy by. A deploy that cannot write it must stop here, not
    // run on and die leaving an untracked work dir behind (§11: a failed write
    // is not a successful deploy).
    return journal_upsert(paths, entry);
  };

  {
    const Result<void> staged = stage("cloning");
    if (staged.is_err())
      return with_context(std::move(staged).error(),
                          "deploy: recording the 'cloning' journal stage");
  }
  log("fetch " + recipe.service_name + "@" + recipe.commit + " port " +
      std::to_string(port));
  Result<void> fetched = ops.fetch_source(recipe, work_dir, paths, options);
  if (fetched.is_err()) {
    return Error(step_error("fetch", std::move(fetched).error().ctx()) +
                 " (journal entry kept for cleanup)");
  }

  {
    const Result<void> staged = stage("building");
    if (staged.is_err())
      return with_context(std::move(staged).error(),
                          "deploy: recording the 'building' journal stage");
  }
  // Stage lines are what the TUI's tail reads to show progress (the log is the
  // engine's, never a second sink the panel owns), so they are written here
  // rather than synthesized from the return value.
  log("build " + recipe.service_name + " port " + std::to_string(port));
  Result<void> prefetched = ops.prefetch(recipe, work_dir, paths, options);
  if (prefetched.is_err()) {
    return Error("deploy: prefetch failed: " +
                 std::move(prefetched).error().ctx());
  }
  Result<void> built = ops.build(recipe, work_dir, port, paths, options);
  if (built.is_err()) {
    return Error("deploy: build failed: " + std::move(built).error().ctx() +
                 " (journal entry kept for cleanup)");
  }

  {
    const Result<void> staged = stage("registering");
    if (staged.is_err())
      return with_context(std::move(staged).error(),
                          "deploy: recording the 'registering' journal stage");
  }
  log("start " + recipe.service_name);
  Result<std::filesystem::path> unit =
      ops.write_unit_file(recipe, work_dir, port, paths);
  if (unit.is_err()) {
    return Error("deploy: unit rendering failed: " +
                 std::move(unit).error().ctx());
  }
  Result<void> started = ops.start_service(recipe.service_name);
  if (started.is_err()) {
    return Error(
        "deploy: unit start failed: " + std::move(started).error().ctx() +
        " (journal entry kept for cleanup)");
  }
  // `systemctl enable --now` returns 0 even for a unit that dies a moment
  // later, so success is only claimed once the unit is actually up (§11:
  // a failure must never read as a success).
  Result<void> running =
      ops.await_running(recipe.service_name, paths, options.cancel);
  if (running.is_err()) {
    return Error(step_error("start", std::move(running).error().ctx()) +
                 " (journal entry kept for cleanup)");
  }
  // `await_running` proves the unit STAYED active; it does not prove the app
  // serves the port this deploy recorded. A fixed-port recipe whose app binds
  // somewhere else would deploy "successfully" against a port nothing listens
  // on (§11), so a live loopback listener is required before success. Same
  // failure semantics as the other start failures: loud, journal kept, not
  // registered.
  Result<void> listening =
      ops.verify_listening(recipe.service_name, port, paths, options.cancel);
  if (listening.is_err()) {
    return Error(step_error("start", std::move(listening).error().ctx()) +
                 " (journal entry kept for cleanup)");
  }

  ServiceRecord record;
  record.service_name = recipe.service_name;
  record.display_name = recipe.display_name;
  record.description = recipe.description;
  record.repo = recipe.owner_repo();
  record.commit = recipe.commit;
  record.port = port;
  Result<void> registered = ops.register_service(record, paths);
  if (registered.is_err()) {
    // Logged as a stage of its own: this is the failure the panel must never
    // render as a success — the unit is live and unregistered (§11).
    log("REGISTRY WRITE FAILED " + recipe.service_name + ": " +
        registered.error().ctx());
    return Error("deploy: REGISTRY WRITE FAILED: " + registered.error().ctx() +
                 " — the unit is live but NOT registered; rerun the deploy");
  }
  Result<void> cleared = journal_remove(paths, recipe.service_name);
  if (cleared.is_err())
    return std::move(cleared).error();
  log("done " + recipe.service_name + " port " + std::to_string(port));

  DeployOutcome outcome;
  outcome.service_name = recipe.service_name;
  outcome.port = port;
  outcome.work_dir = work_dir;
  outcome.unit_path = std::move(unit).value();
  return outcome;
}

}  // namespace berth