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
};

// Registry + live status for every registered service. One bounded
// `systemctl --user is-active` per row, each with its own timeout and the
// caller's cancel flag: one unresponsive unit must not cost the whole list.
[[nodiscard]] Result<std::vector<ServiceRow>> service_rows(
    const Paths& paths, std::atomic<bool>* cancel);

[[nodiscard]] Result<void> service_start(const std::string& service_name);
[[nodiscard]] Result<void> service_stop(const std::string& service_name);

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