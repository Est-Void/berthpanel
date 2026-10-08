// src/berth/service_ops.cpp — see service_ops.h for the contract.

#include "service_ops.h"

#include <chrono>
#include <cstdlib>
#include <system_error>
#include <utility>

#include "../core/names.h"
#include "../core/state.h"
#include "../util/fs.h"
#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/which.h"
#include "units.h"

namespace berth {

namespace {

// Every systemctl round trip here is bounded. The panel's live status list
// must never be able to hang the panel: one unresponsive unit costs its own
// row, not the whole screen (dev.md §5).
constexpr std::chrono::milliseconds kIsActiveTimeout{4000};

// systemctl is-active exit codes are DATA (units.h): 0 active, 3 inactive
// (also "failed" and "unit not found"), 4 unknown.
constexpr int kExitActive = 0;
constexpr int kExitInactive = 3;

constexpr std::size_t kCommitShortLen = 7;

ServiceState state_from_exit_code(const int code) {
  if (code == kExitActive)
    return ServiceState::kActive;
  if (code == kExitInactive)
    return ServiceState::kInactive;
  return ServiceState::kUnknown;
}

std::string commit_short(const std::string& commit) {
  if (commit.size() <= kCommitShortLen)
    return commit;
  return commit.substr(0, kCommitShortLen);
}

Result<void> check_not_cancelled(std::atomic<bool>* cancel,
                                 const std::string& what) {
  if (cancel != nullptr && cancel->load()) {
    return Error("service_ops: cancelled during " + what);
  }
  return {};
}

}  // namespace

std::string_view service_state_label(const ServiceState state) {
  switch (state) {
    case ServiceState::kActive:
      return "active";
    case ServiceState::kInactive:
      return "inactive";
    case ServiceState::kUnknown:
      return "unknown";
  }
  // Unreachable: the enum has no other member, and a new one is a compile
  // error above.
  std::abort();
}

Result<std::filesystem::path> unit_file_for(const Paths& paths,
                                            const std::string& service_name) {
  // THE shared validator (dev.md §11): the unit creator validates, so the
  // remover must too, or a hand-edited registry entry can delete outside the
  // unit directory.
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  return paths.units_dir / (service_name + ".service");
}

Result<std::vector<ServiceRow>> service_rows(const Paths& paths,
                                             std::atomic<bool>* cancel) {
  Result<std::vector<ServiceRecord>> records = registry_list(paths);
  if (records.is_err()) {
    return with_context(std::move(records).error(), "listing services");
  }

  std::vector<ServiceRow> rows;
  rows.reserve(records.value().size());
  for (const ServiceRecord& record : records.value()) {
    Result<void> live = check_not_cancelled(cancel, "listing services");
    if (live.is_err())
      return std::move(live).error();

    ServiceRow row;
    row.service_name = record.service_name;
    row.display_name = record.display_name;
    row.port = record.port;
    row.commit_short = commit_short(record.commit);

    // A name that fails validation still gets a row — it shows up as
    // "unknown" rather than disappearing, because a registry that cannot be
    // listed is not a registry that is empty (§11).
    const Result<int> active =
        systemctl_user_code({"is-active", record.service_name + ".service"},
                            kIsActiveTimeout, cancel);
    if (active.is_err() || active.value() < 0) {
      row.state = ServiceState::kUnknown;
    } else {
      row.state = state_from_exit_code(active.value());
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

Result<void> service_start(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  return with_context(systemctl_user({"start", service_name + ".service"}),
                      "starting " + service_name);
}

Result<void> service_stop(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  return with_context(systemctl_user({"stop", service_name + ".service"}),
                      "stopping " + service_name);
}

Result<void> service_restart(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  // `systemctl restart` exists, but stop-then-start is what the two verbs
  // above already prove, and a restart that silently no-ops when the unit is
  // not loaded would be a lie about a repair.
  Result<void> stopped = service_stop(service_name);
  if (stopped.is_err()) {
    return stopped;
  }
  return with_context(service_start(service_name),
                      "restarting " + service_name);
}

Result<void> service_kill(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  return with_context(
      systemctl_user({"kill", "--signal=SIGKILL", "--kill-who=main",
                      service_name + ".service"}),
      "killing " + service_name);
}

namespace {

// The unit's MainPID, as systemd reports it. One bounded call; "" when the
// unit has none (never started, or failed to start).
Result<std::string> main_pid_of(const std::string& unit) {
  // `show --value` prints on stdout and systemctl_user() throws the output
  // away, so this one call goes through run() directly — with the same argv
  // array and timeout as everything else (§11: never a command string).
  const Result<std::string> systemctl = resolve_executable("systemctl");
  if (systemctl.is_err()) {
    return with_context(std::move(systemctl).error(), "reading " + unit);
  }
  RunLimits limits;
  limits.timeout = kIsActiveTimeout;
  const Result<RunOutput> shown = run({systemctl.value(), "--user", "show",
                                       "--property=MainPID", "--value", unit},
                                      limits);
  if (shown.is_err()) {
    return with_context(std::move(shown).error(), "reading " + unit);
  }
  if (shown.value().exit_code != 0) {
    return Error("service_ops: systemctl show failed for " + unit + ": " +
                 std::string(berth::trim(shown.value().err)));
  }
  return std::string(berth::trim(shown.value().out));
}

}  // namespace

char proc_stat_state(const std::string_view stat_line) {
  // "1234 (some (weird) name) T 1234 ..." — the comm field is in parentheses
  // and may contain both, so the LAST ')' is the only reliable separator.
  const std::size_t close = stat_line.rfind(')');
  // `close + 2` is the index of the state character; it must be strictly
  // inside the view. `>` let a line that ends exactly at the separating space
  // (e.g. "1 (x) ") index one past the end — an OOB read on hostile input.
  if (close == std::string_view::npos || close + 2 >= stat_line.size()) {
    return '\0';
  }
  const char state = stat_line[close + 2];
  // Only the documented state letters are accepted; anything else means the
  // line is not what we think it is.
  static constexpr std::string_view kStates = "RSDZTtWXxKWPslNNI";
  if (kStates.find(state) == std::string_view::npos) {
    return '\0';
  }
  return state;
}

Result<bool> service_is_frozen(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  const Result<std::string> pid = main_pid_of(service_name + ".service");
  if (pid.is_err()) {
    return std::move(pid).error();
  }
  if (pid.value().empty() || pid.value() == "0") {
    return false;  // no process, so nothing is stopped
  }
  const Result<std::string> stat = read_file("/proc/" + pid.value() + "/stat");
  if (stat.is_err()) {
    // The process may have exited between the two calls; that is not a failure
    // of the freeze check, it means there is nothing frozen.
    return false;
  }
  const char state = proc_stat_state(stat.value());
  if (state == '\0') {
    return Error("service_ops: cannot read the process state of " +
                 service_name + " (pid " + pid.value() +
                 ") — treating it as NOT frozen, which is the safe direction");
  }
  return state == 'T' || state == 't';
}

Result<bool> service_freeze(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  Result<void> sent =
      systemctl_user({"kill", "--signal=SIGSTOP", "--kill-who=main",
                      service_name + ".service"});
  if (sent.is_err()) {
    return with_context(std::move(sent).error(), "freezing " + service_name);
  }
  // Sent is not stopped. Confirm against the process itself (§11: a signal
  // that went out is not a state).
  return service_is_frozen(service_name);
}

Result<bool> service_unfreeze(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("service_ops: invalid service name: '" + service_name + "'");
  }
  Result<void> sent =
      systemctl_user({"kill", "--signal=SIGCONT", "--kill-who=main",
                      service_name + ".service"});
  if (sent.is_err()) {
    return with_context(std::move(sent).error(), "unfreezing " + service_name);
  }
  // Same meaning as service_freeze(): true = FROZEN NOW. Two functions whose
  // bool means opposite things is a trap for the caller, and it already was
  // one.
  return service_is_frozen(service_name);
}

Result<void> service_uninstall(const Paths& paths,
                               const std::string& service_name) {
  const Result<std::filesystem::path> unit_file =
      unit_file_for(paths, service_name);
  if (unit_file.is_err())
    return std::move(unit_file).error();

  // The unit file's existence decides whether there is anything to disable.
  // A missing unit is not an error here: the registry entry and the work dir
  // may well outlive it, and uninstall has to be able to finish.
  std::error_code ec;
  const bool unit_present = std::filesystem::exists(unit_file.value(), ec);
  if (ec) {
    return Error("service_ops: checking " + unit_file.value().string() + ": " +
                 ec.message());
  }

  if (unit_present) {
    // systemctl_disable is stop+disable+daemon-reload in one, and it
    // validates the name itself.
    Result<void> disabled = systemctl_disable(service_name);
    if (disabled.is_err()) {
      return with_context(std::move(disabled), "disabling " + service_name);
    }
  }

  std::error_code remove_ec;
  std::filesystem::remove(unit_file.value(), remove_ec);
  if (remove_ec) {
    return Error("service_ops: removing " + unit_file.value().string() + ": " +
                 remove_ec.message());
  }

  Result<void> reloaded = systemctl_daemon_reload();
  if (reloaded.is_err())
    return with_context(std::move(reloaded),
                        "after uninstalling " + service_name);

  Result<void> unregistered = registry_unregister(paths, service_name);
  if (unregistered.is_err()) {
    return with_context(std::move(unregistered),
                        "after uninstalling " + service_name);
  }

  // The work dir under paths.services_dir stays: it holds the user's data,
  // and a silent delete is forbidden. Removing it is a separate, explicit
  // act (dev.md §12 open purge policy).
  return {};
}

}  // namespace berth