#include "deploy.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <functional>
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

}
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
  // Logged BEFORE the clone so the line exists even if it dies mid-clone.
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
      // Replacing the tree under a LIVE service is a surprise, not a repair.
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
    // Unregistered ⇒ interrupted leftover: remove it and SAY so.
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
    const std::uint16_t port, const Paths& paths,
    const std::vector<std::string>& after_units,
    const std::vector<std::string>& wants_units) {
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
  spec.after_units = after_units;
  spec.wants_units = wants_units;
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

std::string step_error(const std::string& step, std::string ctx) {
  if (ctx.rfind("deploy: ", 0) == 0) {
    return ctx;
  }
  return "deploy: " + step + ": " + std::move(ctx);
}

}
std::string parse_systemctl_show(const std::string_view show) {
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
    // The cap can land mid-line: drop the partial head, never start mid-word.
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

// Cannot fail: a missing or timed-out tool skips its part.
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

}
Result<void> RealDeployOps::await_running(const std::string& service_name,
                                          const Paths& paths,
                                          std::atomic<bool>* cancel) {
  Result<std::string> active = wait_for_active_unit(
      service_name, std::chrono::milliseconds(8000), cancel);
  if (active.is_err()) {
    return std::move(active).error();
  }
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
      break;
    }
    interruptible_sleep(std::chrono::milliseconds(250));
  }
  Result<std::string> settled = settle_active(service_name, observed);
  if (settled.is_err()) {
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
    // /proc/net only — the dumb, unattributed reader: no /proc/<pid> walk.
    Result<PortReport> report = listening_ports(paths);
    if (report.is_err()) {
      // A probe that cannot run is not evidence of absence: warn, never fail.
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
    // Vacuous truth is not evidence: an unanswered window confirmed nothing.
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
      // The start_cmd is an assumption that says so: its verdict rides along.
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

// Bounded: a busy host would push the actual diagnosis out of the error line.
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

}
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
      return {};
    }
    listening_on_port = true;
    if (!port.wildcard) {
      wildcard_only = false;  // a specific non-loopback bind, not the wildcard
    }
  }
  const std::string observed = render_observed_listeners(report.ports);
  if (!listening_on_port) {
    // Active but nothing on the recorded port: the silent wrong answer.
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
  // Bound beyond loopback is the exposure the loopback-only invariant forbids.
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

// True unless entry.work_dir resolves STRICTLY inside services_dir — per
// component, never as a string prefix, after weakly_canonical. Error = outside.
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

// remove(2) does not follow the symlink and ignores a missing path.
Result<bool> remove_unit_artifact(const std::filesystem::path& path) {
  std::error_code ec;
  const bool removed = std::filesystem::remove(path, ec);
  if (ec)
    return Error("remove " + path.string() + ": " + ec.message());
  return removed;
}

}
Result<ReconcileReport> sweep_interrupted_entries(
    const Paths& paths, DeployOps& ops,
    const std::vector<JournalEntry>& entries, std::atomic<bool>* cancel) {
  // PRECONDITION (deploy.h): the lock is held, so an entry present means
  // interrupted — unless it is in the registry, i.e. only the write-back was
  // lost. Read once: if it fails, nothing is provably unregistered.
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

    // Path guard: a work_dir outside THIS services_dir may be another install.
    if (work_dir_outside_services(paths, entry)) {
      log_sweep(paths, "cleanup: " + entry.service_name + " work_dir '" +
                           entry.work_dir + "' is outside " +
                           paths.services_dir.string() + " — nothing deleted");
      report.skipped_outside.push_back(entry.service_name);
      continue;
    }

    if (!work_dir_matches_layout(paths, entry)) {
      fail("journal work_dir '" + entry.work_dir + "' is not " +
           (paths.services_dir / entry.service_name).string());
      continue;
    }

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

    // 1. The app unit may be LIVE: a deploy that died at 'registering' started
    // it before the registry write was confirmed.
    Result<bool> stopped = ops.stop_unit(entry.service_name, cancel);
    if (stopped.is_err()) {
      ok = false;
      why = stopped.error().ctx();
    } else if (stopped.value()) {
      log_sweep(paths, "cleanup stopped unit " + entry.service_name);
    }

    // 2. The orphaned build unit; build_unit_name() gates it and validates it.
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

    // 4. The unit file AND the `enable` symlink: removing only the unit file
    // leaves the dangling link. No daemon-reload: a fileless unit is inert.
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

    // Secrets stay: user env values, rewritten on the next deploy.

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
        .service_name = entry.service_name,
        .stage = entry.stage,
        .stack = entry.stack});
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

  // The lock, not timestamps, decides liveness.
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
           swept.stage + "'";
    if (!swept.stack.empty()) {
      out += ", stack " + swept.stack;
    }
    out += ")\n";
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

namespace {

using StageWriter = std::function<Result<void>(const char* name)>;
using StageLogger = std::function<void(const std::string& line)>;

Result<void> write_stage(const Paths& paths, const std::string& service_name,
                         const std::string& work_dir, const std::string& repo,
                         const std::string& commit, const std::string& stack,
                         const char* name) {
  JournalEntry entry;
  entry.service_name = service_name;
  entry.work_dir = work_dir;
  entry.repo = repo;
  entry.commit = commit;
  entry.stage = name;
  entry.stack = stack;
  entry.started_at_unix = static_cast<std::int64_t>(::time(nullptr));
  // Cannot write the journal ⇒ stop here, never leave an untracked work dir.
  return journal_upsert(paths, entry);
}

Result<void> preflight_unit(const Recipe& recipe,
                            const DeployOptions& options) {
  Result<std::vector<ToolStatus>> tools =
      check_tools(recipe.tools, options.cancel);
  if (tools.is_err())
    return std::move(tools).error();
  const Result<PreflightReport> preflight =
      preflight_recipe(recipe_view(recipe), tools.value());
  if (preflight.is_err())
    return std::move(preflight).error();
  if (!preflight.value().ready()) {
    return Error("deploy: preflight blocked " + recipe.service_name + ":\n" +
                 render_preflight(preflight.value()));
  }
  return {};
}

Result<DeployOutcome> deploy_unit(const Paths& paths, const Recipe& recipe,
                                  const std::uint16_t port,
                                  const std::filesystem::path& work_dir,
                                  DeployOps& ops,
                                  const DeployOptions& options,
                                  const StageWriter& stage,
                                  const StageLogger& log,
                                  const std::vector<std::string>& after_units =
                                      {},
                                  const std::vector<std::string>& wants_units =
                                      {}) {
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
  // The TUI tail reads these stage lines; the log is the engine's only sink.
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
      ops.write_unit_file(recipe, work_dir, port, paths, after_units,
                          wants_units);
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
  Result<void> running =
      ops.await_running(recipe.service_name, paths, options.cancel);
  if (running.is_err()) {
    return Error(step_error("start", std::move(running).error().ctx()) +
                 " (journal entry kept for cleanup)");
  }
  // await_running does not prove the app serves the recorded port. Same
  // semantics as the other start failures: loud, journal kept, unregistered.
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
    // Its own stage: the unit is live and unregistered. Never a success.
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

StageLogger make_log(const Paths& paths) {
  return [&paths](const std::string& line) {
    const Result<void> logged = deploylog_append(paths, line);
    static_cast<void>(logged);
  };
}

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

  const Result<void> ready = preflight_unit(recipe, options);
  if (ready.is_err())
    return std::move(ready).error();

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
  const StageWriter stage =
      [&paths, &recipe, &work_dir](const char* name) -> Result<void> {
    return write_stage(paths, recipe.service_name, work_dir.string(),
                       recipe.owner_repo(), recipe.commit, "", name);
  };
  return deploy_unit(paths, recipe, port, work_dir, ops, options, stage,
                     make_log(paths));
}

Result<StackDeployOutcome> run_stack_deploy(const Paths& paths,
                                            const Stack& stack, DeployOps& ops,
                                            const DeployOptions& options) {
  const Result<void> analysed = validate_stack(stack);
  if (analysed.is_err())
    return std::move(analysed).error();

  Result<DeployLock> lock = DeployLock::acquire(paths);
  if (lock.is_err())
    return std::move(lock).error();

  Result<std::vector<JournalEntry>> stale = journal_load(paths);
  if (stale.is_err())
    return std::move(stale).error();
  Result<ReconcileReport> swept =
      sweep_interrupted_entries(paths, ops, stale.value(), options.cancel);
  if (swept.is_err())
    return std::move(swept).error();

  std::vector<std::string> member_units;
  member_units.reserve(stack.members.size());
  for (const StackMember& member : stack.members) {
    Result<std::string> unit =
        member_unit_name(stack.service_name, member.name);
    if (unit.is_err())
      return std::move(unit).error();
    member_units.push_back(std::move(unit).value());
  }
  for (const std::pair<std::string, std::string>& failure :
       swept.value().failed) {
    static_cast<void>(deploylog_append(
        paths, "cleanup failed for " + failure.first + ": " + failure.second));
    if (std::find(member_units.begin(), member_units.end(), failure.first) !=
        member_units.end()) {
      return Error("deploy: interrupted deploy of stack " +
                   stack.service_name + " member " + failure.first +
                   " could not be cleaned up, refusing to start the stack: " +
                   failure.second);
    }
  }

  for (const StackMember& member : stack.members) {
    const Result<void> ready = preflight_unit(member.recipe, options);
    if (ready.is_err())
      return std::move(ready).error();
  }

  Result<std::vector<ServiceRecord>> registered = registry_list(paths);
  if (registered.is_err())
    return std::move(registered).error();
  std::vector<std::uint16_t> taken;
  for (const ServiceRecord& record : registered.value()) {
    if (record.port != 0)
      taken.push_back(record.port);
  }
  std::vector<std::uint16_t> ports(stack.members.size(), 0);
  for (std::size_t i = 0; i < stack.members.size(); ++i) {
    if (stack.members[i].recipe.port != 0) {
      ports[i] = stack.members[i].recipe.port;
      taken.push_back(ports[i]);
      continue;
    }
    Result<std::uint16_t> picked = pick_free_port(taken);
    if (picked.is_err())
      return std::move(picked).error();
    ports[i] = picked.value();
    taken.push_back(ports[i]);
  }

  // Write EVERY member entry before the first member runs, so a kill anywhere
  // leaves the whole group for cleanup, not only the member in progress.
  const std::int64_t now = static_cast<std::int64_t>(::time(nullptr));
  std::vector<JournalEntry> entries;
  entries.reserve(stack.members.size());
  for (std::size_t i = 0; i < stack.members.size(); ++i) {
    JournalEntry entry;
    entry.service_name = member_units[i];
    entry.work_dir = (paths.services_dir / member_units[i]).string();
    entry.repo = stack.members[i].recipe.owner_repo();
    entry.commit = stack.members[i].recipe.commit;
    entry.stage = "cloning";
    entry.stack = stack.service_name;
    entry.started_at_unix = now;
    entries.push_back(std::move(entry));
  }
  const Result<void> recorded = journal_upsert_many(paths, entries);
  if (recorded.is_err()) {
    return with_context(std::move(recorded).error(),
                        "deploy: recording the stack journal entries");
  }

  const Result<std::vector<std::size_t>> order = stack_deploy_order(stack);
  if (order.is_err())
    return std::move(order).error();
  const StageLogger log = make_log(paths);
  StackDeployOutcome outcome;
  outcome.stack_name = stack.service_name;
  for (const std::size_t i : order.value()) {
    const StackMember& stack_member = stack.members[i];
    const Recipe& recipe = stack_member.recipe;
    const std::filesystem::path work_dir = paths.services_dir / member_units[i];
    const StageWriter stage =
        [&paths, &recipe, &stack, &member_units, i](
            const char* name) -> Result<void> {
      return write_stage(paths, member_units[i],
                         (paths.services_dir / member_units[i]).string(),
                         recipe.owner_repo(), recipe.commit, stack.service_name,
                         name);
    };
    // Sibling ids become validated unit stems; render_unit re-checks them.
    std::vector<std::string> after_units;
    after_units.reserve(stack_member.after.size());
    for (const std::string& dep : stack_member.after) {
      Result<std::string> dep_unit = member_unit_name(stack.service_name, dep);
      if (dep_unit.is_err())
        return std::move(dep_unit).error();
      after_units.push_back(std::move(dep_unit).value());
    }
    std::vector<std::string> wants_units;
    wants_units.reserve(stack_member.wants.size());
    for (const std::string& dep : stack_member.wants) {
      Result<std::string> dep_unit = member_unit_name(stack.service_name, dep);
      if (dep_unit.is_err())
        return std::move(dep_unit).error();
      wants_units.push_back(std::move(dep_unit).value());
    }
    Result<DeployOutcome> deployed =
        deploy_unit(paths, recipe, ports[i], work_dir, ops, options, stage, log,
                    after_units, wants_units);
    if (deployed.is_err())
      return std::move(deployed).error();
    outcome.members.push_back(std::move(deployed).value());
  }
  return outcome;
}

}
