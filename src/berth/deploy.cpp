#include "deploy.h"

#include <cerrno>
#include <ctime>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

Result<std::vector<std::string>> start_argv(
    const Recipe& recipe, const std::filesystem::path& work_dir,
    const std::uint16_t port, std::string& binary) {
  const Result<std::vector<std::string>> tokens =
      split_command_template(recipe.start_cmd);
  if (tokens.is_err())
    return std::move(tokens).error();
  binary = (work_dir / recipe.binary_name).string();
  std::vector<std::pair<std::string, std::string>> values;
  values.emplace_back("bin", binary);
  values.emplace_back("project", work_dir.string());
  values.emplace_back("port", std::to_string(port));
  values.emplace_back("self", binary);
  std::vector<std::string> resolved;
  for (const std::string& token : tokens.value()) {
    const Result<std::string> substituted =
        substitute_placeholders(token, values);
    if (substituted.is_err())
      return std::move(substituted).error();
    resolved.push_back(std::move(substituted).value());
  }
  binary = resolved.front();
  resolved.erase(resolved.begin());
  return resolved;
}

}  // namespace

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
                          .executable = true};
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
      start_argv(recipe, work_dir, port, binary);
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

Result<void> RealDeployOps::await_running(const std::string& service_name,
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
    return std::move(settled).error();
  }
  return {};
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
                                  const std::vector<std::string>& states) {
  if (states.empty()) {
    // Vacuous truth is not evidence: a settle window that never got an answer
    // has confirmed nothing, and must not report a service as up.
    return Error("deploy: " + service_name +
                 " was never confirmed active after starting — no answer from "
                 "systemctl during the settle window");
  }
  for (const std::string& state : states) {
    if (state != "active") {
      // The most common cause by far is a start_cmd the app does not
      // understand — the detector's is an ASSUMPTION and says so — so the
      // error says that instead of leaving the user to read unit logs.
      return Error("deploy: " + service_name +
                   " came up and then left 'active' (state: " + state +
                   ") — it is not serving. A start_cmd the app does not "
                   "understand is the usual cause; the recipe file is the "
                   "recipe to edit.");
    }
  }
  return std::string("active");
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
    //    already written. No daemon-reload: a loaded-but-fileless unit is
    //    inert, and the next enable does a reload anyway.
    if (ok) {
      const std::filesystem::path unit_file =
          paths.units_dir / (entry.service_name + ".service");
      std::error_code ec;
      const bool existed = std::filesystem::exists(unit_file, ec);
      if (ec) {
        ok = false;
        why = "stat " + unit_file.string() + ": " + ec.message();
      } else if (existed) {
        std::filesystem::remove(unit_file, ec);
        if (ec) {
          ok = false;
          why = "remove " + unit_file.string() + ": " + ec.message();
        } else {
          log_sweep(paths, "cleanup removed unit file " + unit_file.string());
        }
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
  Result<void> running = ops.await_running(recipe.service_name, options.cancel);
  if (running.is_err()) {
    return Error(step_error("start", std::move(running).error().ctx()) +
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
        std::move(registered).error().ctx());
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