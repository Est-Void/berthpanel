#pragma once

// Every function here BLOCKS: pass a cancel flag and run them on a worker.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

// kUnknown ≠ kInactive on purpose: an unloaded transient unit exits 4.
enum class ServiceState {
  kActive,
  kInactive,
  kUnknown,
};

[[nodiscard]] std::string_view service_state_label(ServiceState state);

struct ServiceRow {
  std::string service_name;
  std::string display_name;
  ServiceState state = ServiceState::kUnknown;
  std::uint16_t port = 0;
  std::string commit_short;
  // SIGSTOP'd: a signal SENT is not a state — read SubState back to confirm.
  bool frozen = false;
};

[[nodiscard]] Result<std::vector<ServiceRow>> service_rows(
    const Paths& paths, std::atomic<bool>* cancel);

[[nodiscard]] Result<void> service_start(const std::string& service_name);
[[nodiscard]] Result<void> service_stop(const std::string& service_name);

// Keeps the port, so the caller does NOT re-render the unit file.
[[nodiscard]] Result<void> service_restart(const std::string& service_name);

// SIGKILL: unflushed state is gone; nothing returns unless Restart= says so.
[[nodiscard]] Result<void> service_kill(const std::string& service_name);

// BOTH return true = FROZEN NOW; a frozen process keeps its port and locks.
[[nodiscard]] Result<bool> service_freeze(const std::string& service_name);
[[nodiscard]] Result<bool> service_unfreeze(const std::string& service_name);

// systemd still reports SubState=running for a SIGSTOPped service; /proc rules.
[[nodiscard]] Result<bool> service_is_frozen(const std::string& service_name);

// State is the char after the LAST ')'; '\0' when unparseable, never "running".
[[nodiscard]] char proc_stat_state(std::string_view stat_line);

// The work dir is DELIBERATELY left: it holds user data. No silent deletes.
[[nodiscard]] Result<void> service_uninstall(const Paths& paths,
                                             const std::string& service_name);

// The only route to a unit file, so nothing escapes validate_unit_name.
[[nodiscard]] Result<std::filesystem::path> unit_file_for(
    const Paths& paths, const std::string& service_name);

}
