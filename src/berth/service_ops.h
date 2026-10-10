#pragma once

// src/berth/service_ops.h — lifecycle operations over an ALREADY DEPLOYED
// service: the registry + live status list, start, stop, uninstall.
//
// This module is the engine, not the TUI: it is callable from the CLI and
// from tests, and it must never learn what a screen is (dev.md §5 layering).
// Two consequences the header owns:
//
//   * EVERY function here blocks. The TUI is forbidden from calling any of
//     them on its render thread (dev.md §5, §11: the old panel froze solid on
//     `systemctl`). Callers pass a cancel flag and run them on a worker.
//   * `is-active` exit codes are DATA. `systemctl is-active` exits 3 for
//     "inactive", and that is not a broken systemctl — see
//     systemctl_user_code() in units.h for why the code is returned as a
//     value rather than as an Err.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

// The three answers `systemctl --user is-active` can give that matter here.
// kUnknown is deliberately distinct from kInactive: an unloaded transient
// unit exits 4 ("unknown"), and reporting that as "inactive" would claim to
// know something systemctl did not.
enum class ServiceState {
  kActive,    // is-active exited 0
  kInactive,  // is-active exited 3 — a real answer, not a failure
  kUnknown,   // some other exit (4 = unknown), a cancellation, or a broken
              // systemctl
};

[[nodiscard]] std::string_view service_state_label(ServiceState state);

struct ServiceRow {
  std::string service_name;
  std::string display_name;
  ServiceState state = ServiceState::kUnknown;
  std::uint16_t port = 0;
  std::string commit_short;  // first 7 hex chars, or empty if unpinned
  // SIGSTOP'd by the user. systemd has NO per-unit pause verb (it has none at
  // all: `systemctl pause` is not a verb), so a freeze is a signal, and a
  // signal that was SENT is not a state — every freeze/unfreeze reads the
  // unit's SubState back to confirm what happened (§11).
  bool frozen = false;
};

// Registry + live status for every registered service. One bounded
// `systemctl --user is-active` per row, each with its own timeout and the
// caller's cancel flag: one unresponsive unit must not cost the whole list.
[[nodiscard]] Result<std::vector<ServiceRow>> service_rows(
    const Paths& paths, std::atomic<bool>* cancel);

[[nodiscard]] Result<void> service_start(const std::string& service_name);
[[nodiscard]] Result<void> service_stop(const std::string& service_name);

// stop + start, each through the same validated verb and timeout as the
// standalone ones. A restart is a repair that keeps the port, so the caller
// does NOT re-render the unit file.
[[nodiscard]] Result<void> service_restart(const std::string& service_name);

// SIGKILL to the unit's main process. DESTRUCTIVE: whatever the service had
// not flushed is gone (a database mid-write does not survive this), and
// nothing comes back unless the unit itself says Restart=. The panel asks
// first; nothing in this function asks on its behalf.
[[nodiscard]] Result<void> service_kill(const std::string& service_name);

// Freeze / unfreeze: SIGSTOP and SIGCONT to the unit's main process, then the
// state is read BACK. BOTH return the SAME thing — true = the service is
// frozen right now — because a pair of functions whose bools mean opposite
// things is a trap (and it was one: the panel read unfreeze's "it started
// again" as "it is frozen" and left the row frozen while the service ran).
//
// This is NOT a suspend: a frozen process keeps its port, its memory and its
// locks, which is why the panel says "freeze" rather than "pause" and tells the
// user the port stays bound.
[[nodiscard]] Result<bool> service_freeze(const std::string& service_name);
[[nodiscard]] Result<bool> service_unfreeze(const std::string& service_name);

// Is the unit's MAIN process stopped right now? systemd does not track this:
// it reports SubState=running for a SIGSTOPped service (checked against the
// live user manager), so the only honest instrument is the process's own state
// flag in /proc.
[[nodiscard]] Result<bool> service_is_frozen(const std::string& service_name);

// PURE: the state field of a /proc/<pid>/stat line — 'T' for a stopped
// process. The comm field can contain BOTH spaces and parentheses, so the state
// is the character after the LAST ')'. '\0' when the line is not parseable:
// a /proc line that changed shape must not read as "running".
[[nodiscard]] char proc_stat_state(std::string_view stat_line);

// stop + disable, delete the unit file (paths.units_dir only, behind the
// shared validate_unit_name), daemon-reload, then unregister.
//
// The work dir under paths.services_dir is DELIBERATELY left in place: it
// holds the user's data (databases, uploads), and a silent delete is
// forbidden. A separate explicit purge policy is future work (dev.md §12).
[[nodiscard]] Result<void> service_uninstall(const Paths& paths,
                                             const std::string& service_name);

// The pure part, exposed so the path the remover builds is testable without a
// live systemd: units_dir / <name>.service, or an Err naming the violation.
// There is no way to reach a unit file that did not come through here.
[[nodiscard]] Result<std::filesystem::path> unit_file_for(
    const Paths& paths, const std::string& service_name);

}  // namespace berth