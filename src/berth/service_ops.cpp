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

// Bounded: one unresponsive unit costs its own row, not the whole screen.
constexpr std::chrono::milliseconds kIsActiveTimeout{4000};

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

}
std::string_view service_state_label(const ServiceState state) {
  switch (state) {
    case ServiceState::kActive:
      return "active";
    case ServiceState::kInactive:
      return "inactive";
    case ServiceState::kUnknown:
      return "unknown";
  }
  std::abort();
}

Result<std::filesystem::path> unit_file_for(const Paths& paths,
                                            const std::string& service_name) {
  // The shared validator guards the remover too: a registry is hand-editable.
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

    // A bad name still gets a row: an unlistable registry is not an empty one.
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
  // stop+start, not `restart`: it silently no-ops when the unit is not loaded.
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

Result<std::string> main_pid_of(const std::string& unit) {
  // systemctl_user() discards stdout, so this goes through run() directly.
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

}
char proc_stat_state(const std::string_view stat_line) {
  const std::size_t close = stat_line.rfind(')');
  // `>=` not `>`: a line ending at the space would read one past the end.
  if (close == std::string_view::npos || close + 2 >= stat_line.size()) {
    return '\0';
  }
  const char state = stat_line[close + 2];
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
    return false;
  }
  const Result<std::string> stat = read_file("/proc/" + pid.value() + "/stat");
  if (stat.is_err()) {
    // Exited between the two calls: nothing is frozen, so not a failure.
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
  // Sent is not stopped: confirm against the process itself.
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
  // Same meaning as service_freeze(): true = FROZEN NOW.
  return service_is_frozen(service_name);
}

Result<void> service_uninstall(const Paths& paths,
                               const std::string& service_name) {
  const Result<std::filesystem::path> unit_file =
      unit_file_for(paths, service_name);
  if (unit_file.is_err())
    return std::move(unit_file).error();

  // A missing unit is not an error: the registry entry and work dir outlive it.
  std::error_code ec;
  const bool unit_present = std::filesystem::exists(unit_file.value(), ec);
  if (ec) {
    return Error("service_ops: checking " + unit_file.value().string() + ": " +
                 ec.message());
  }

  if (unit_present) {
    // stop+disable+reload in one; it validates the name itself.
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

  return {};
}

}
