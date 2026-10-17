#pragma once

// __repair (stage 10.3) is a READ-ONLY doctor over the four sources of truth:
// registry, unit files (with enable links and live state), journal and work
// dirs. It fixes nothing; every finding carries the command that would.
// Liveness is injected, so tests compare fixtures without a live systemd.

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../core/paths.h"
#include "../core/state.h"
#include "../util/result.h"
#include "journal.h"

namespace berth {

enum class RepairIssue {
  kRegisteredUnitMissing,
  kRegisteredUnitInactive,
  kRegisteredNotEnabled,
  kRegisteredWorkDirMissing,
  kUnitUnregistered,
  kUnitLiveUnregistered,
  kEnableLinkDangling,
  kWorkDirOrphan,
  kJournalInterrupted,
  kJournalStaleRegistered,
  kJournalUnsafeWorkDir,
  kDeployInProgress,
};

struct RepairFinding {
  RepairIssue issue = RepairIssue::kRegisteredUnitMissing;
  std::string service_name;  // empty only for kDeployInProgress
  std::string stack;         // group stem when a journal entry carries one
  std::string detail;
  std::string repair;  // the command a human may run; never executed here
};

struct RepairSnapshot {
  std::vector<ServiceRecord> registry;
  std::vector<JournalEntry> journal;
  // Journal names whose work_dir fails the sweep's prefix/layout guard.
  std::vector<std::string> journal_unsafe;
  std::vector<std::string> unit_files;
  std::vector<std::string> enabled_units;
  std::vector<std::string> work_dirs;
  std::vector<std::string> live_units;
  bool lock_busy = false;
};

struct RepairReport {
  std::vector<RepairFinding> findings;
};

// True => the unit is active. Err => liveness could not be determined.
using UnitLiveProbe =
    std::function<Result<bool>(const std::string& service_name)>;

[[nodiscard]] UnitLiveProbe systemd_unit_probe();

[[nodiscard]] Result<RepairSnapshot> collect_repair_snapshot(
    const Paths& paths, const UnitLiveProbe& probe);

[[nodiscard]] RepairReport repair_check(const RepairSnapshot& snapshot);

[[nodiscard]] std::string render_repair_report(const RepairReport& report);

[[nodiscard]] nlohmann::json repair_json(const RepairReport& report);

}
