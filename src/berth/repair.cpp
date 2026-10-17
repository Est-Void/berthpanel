#include "repair.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../core/jsonout.h"
#include "../core/names.h"
#include "../util/errno.h"
#include "../util/fd.h"
#include "lock.h"
#include "proxy.h"
#include "units.h"

namespace berth {

namespace {

const char* issue_name(const RepairIssue issue) {
  switch (issue) {
    case RepairIssue::kRegisteredUnitMissing:
      return "registered-unit-missing";
    case RepairIssue::kRegisteredUnitInactive:
      return "registered-unit-inactive";
    case RepairIssue::kRegisteredNotEnabled:
      return "registered-not-enabled";
    case RepairIssue::kRegisteredWorkDirMissing:
      return "registered-work-dir-missing";
    case RepairIssue::kUnitUnregistered:
      return "unit-unregistered";
    case RepairIssue::kUnitLiveUnregistered:
      return "unit-live-unregistered";
    case RepairIssue::kEnableLinkDangling:
      return "enable-link-dangling";
    case RepairIssue::kWorkDirOrphan:
      return "work-dir-orphan";
    case RepairIssue::kJournalInterrupted:
      return "journal-interrupted";
    case RepairIssue::kJournalStaleRegistered:
      return "journal-stale-registered";
    case RepairIssue::kJournalUnsafeWorkDir:
      return "journal-unsafe-work-dir";
    case RepairIssue::kDeployInProgress:
      return "deploy-in-progress";
  }
  std::abort();
}

void add_finding(RepairReport* report, const RepairIssue issue,
                 std::string service, std::string detail, std::string repair,
                 std::string stack = {}) {
  report->findings.push_back(RepairFinding{.issue = issue,
                                           .service_name = std::move(service),
                                           .stack = std::move(stack),
                                           .detail = std::move(detail),
                                           .repair = std::move(repair)});
}

bool panel_owned(const std::string& stem) {
  return stem == kCleanupUnitStem || stem == kCaddyUnitStem;
}

bool strictly_inside(const std::filesystem::path& root,
                     const std::filesystem::path& candidate) {
  auto r = root.begin();
  auto c = candidate.begin();
  for (; r != root.end(); ++r, ++c) {
    if (c == candidate.end() || *c != *r)
      return false;
  }
  return c != candidate.end();  // equal is not strictly inside
}

// Mirrors deploy.cpp's sweep guards: the doctor must agree with cleanup about
// what it is allowed to touch (§11).
bool work_dir_outside(const Paths& paths, const std::string& work_dir) {
  std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(paths.services_dir, ec);
  if (ec)
    return true;
  const auto work =
      std::filesystem::weakly_canonical(std::filesystem::path(work_dir), ec);
  if (ec)
    return true;
  return !strictly_inside(root, work);
}

bool work_dir_matches(const Paths& paths, const std::string& service_name,
                      const std::string& work_dir) {
  const std::filesystem::path expected =
      (paths.services_dir / service_name).lexically_normal();
  return std::filesystem::path(work_dir).lexically_normal() == expected;
}

Result<std::vector<std::string>> service_units_in(const Paths& paths) {
  std::vector<std::string> names;
  std::error_code ec;
  if (!std::filesystem::exists(paths.units_dir, ec)) {
    if (ec) {
      return Error("repair: stat " + paths.units_dir.string() + ": " +
                   ec.message());
    }
    return names;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(paths.units_dir, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec) || ec)
      continue;
    const std::filesystem::path& path = entry.path();
    if (path.extension() != ".service")
      continue;
    const std::string stem = path.stem().string();
    if (!validate_unit_name(stem) || panel_owned(stem))
      continue;
    names.push_back(stem);
  }
  if (ec) {
    return Error("repair: scan " + paths.units_dir.string() + ": " +
                 ec.message());
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<std::vector<std::string>> enabled_units_in(const Paths& paths) {
  std::vector<std::string> names;
  const std::filesystem::path dir = paths.units_dir / "default.target.wants";
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    if (ec) {
      return Error("repair: stat " + dir.string() + ": " + ec.message());
    }
    return names;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(dir, ec)) {
    if (ec)
      break;
    if (!entry.is_symlink(ec) && !entry.is_regular_file(ec))
      continue;
    if (ec)
      continue;
    const std::filesystem::path& path = entry.path();
    if (path.extension() != ".service")
      continue;
    const std::string stem = path.stem().string();
    if (!validate_unit_name(stem) || panel_owned(stem))
      continue;
    names.push_back(stem);
  }
  if (ec) {
    return Error("repair: scan " + dir.string() + ": " + ec.message());
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<std::vector<std::string>> work_dirs_in(const Paths& paths) {
  std::vector<std::string> names;
  std::error_code ec;
  if (!std::filesystem::exists(paths.services_dir, ec)) {
    if (ec) {
      return Error("repair: stat " + paths.services_dir.string() + ": " +
                   ec.message());
    }
    return names;
  }
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(paths.services_dir, ec);
  if (ec) {
    return Error("repair: resolve " + paths.services_dir.string() + ": " +
                 ec.message());
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(paths.services_dir, ec)) {
    if (ec)
      break;
    if (!entry.is_directory(ec) || ec)
      continue;
    const std::string stem = entry.path().filename().string();
    if (!validate_unit_name(stem))
      continue;  // not ours (prefix guard)
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(entry.path(), ec);
    if (ec)
      continue;  // vanished mid-walk
    if (!strictly_inside(root, resolved))
      continue;  // escaping symlink: never followed
    names.push_back(stem);
  }
  if (ec) {
    return Error("repair: scan " + paths.services_dir.string() + ": " +
                 ec.message());
  }
  std::sort(names.begin(), names.end());
  return names;
}

// Read-only flock probe on the EXISTING deploy.lock: same file, same kernel
// lock as DeployLock, but never O_CREAT — __repair writes nothing.
Result<bool> deploy_lock_busy(const Paths& paths) {
  const FdGuard fd(
      ::open(paths.deploy_lock.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    if (errno == ENOENT)
      return false;  // no lock file => no deploy can be holding it
    return Error("repair: open " + paths.deploy_lock.string() + ": " +
                 errno_text(errno));
  }
  if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK)
      return true;
    return Error("repair: flock " + paths.deploy_lock.string() + ": " +
                 errno_text(errno));
  }
  return false;
}

}

UnitLiveProbe systemd_unit_probe() {
  return [](const std::string& service_name) -> Result<bool> {
    Result<int> code =
        systemctl_user_code({"is-active", service_name + ".service"},
                            std::chrono::milliseconds(4000), nullptr);
    if (code.is_err())
      return std::move(code).error();
    return code.value() == 0;
  };
}

Result<RepairSnapshot> collect_repair_snapshot(
    const Paths& paths, const UnitLiveProbe& probe) {
  RepairSnapshot snapshot;
  Result<std::vector<ServiceRecord>> registry = registry_list(paths);
  if (registry.is_err())
    return std::move(registry).error();
  snapshot.registry = std::move(registry).value();

  Result<std::vector<JournalEntry>> journal = journal_load(paths);
  if (journal.is_err())
    return std::move(journal).error();
  snapshot.journal = std::move(journal).value();

  Result<std::vector<std::string>> units = service_units_in(paths);
  if (units.is_err())
    return std::move(units).error();
  snapshot.unit_files = std::move(units).value();

  Result<std::vector<std::string>> enabled = enabled_units_in(paths);
  if (enabled.is_err())
    return std::move(enabled).error();
  snapshot.enabled_units = std::move(enabled).value();

  Result<std::vector<std::string>> work = work_dirs_in(paths);
  if (work.is_err())
    return std::move(work).error();
  snapshot.work_dirs = std::move(work).value();

  for (const JournalEntry& entry : snapshot.journal) {
    if (work_dir_outside(paths, entry.work_dir) ||
        !work_dir_matches(paths, entry.service_name, entry.work_dir)) {
      snapshot.journal_unsafe.push_back(entry.service_name);
    }
  }

  std::set<std::string> probe_names(snapshot.unit_files.begin(),
                                    snapshot.unit_files.end());
  for (const ServiceRecord& record : snapshot.registry)
    probe_names.insert(record.service_name);
  for (const std::string& name : probe_names) {
    Result<bool> live = probe(name);
    if (live.is_err()) {
      return with_context(std::move(live).error(), "repair: probing " + name);
    }
    if (live.value())
      snapshot.live_units.push_back(name);
  }
  std::sort(snapshot.live_units.begin(), snapshot.live_units.end());

  Result<bool> busy = deploy_lock_busy(paths);
  if (busy.is_err())
    return std::move(busy).error();
  snapshot.lock_busy = busy.value();
  return snapshot;
}

RepairReport repair_check(const RepairSnapshot& snapshot) {
  RepairReport report;
  std::set<std::string> registry_names;
  for (const ServiceRecord& record : snapshot.registry)
    registry_names.insert(record.service_name);
  std::set<std::string> journal_names;
  for (const JournalEntry& entry : snapshot.journal)
    journal_names.insert(entry.service_name);
  const std::set<std::string> journal_unsafe(snapshot.journal_unsafe.begin(),
                                             snapshot.journal_unsafe.end());
  const std::set<std::string> units(snapshot.unit_files.begin(),
                                    snapshot.unit_files.end());
  const std::set<std::string> enabled(snapshot.enabled_units.begin(),
                                      snapshot.enabled_units.end());
  const std::set<std::string> work(snapshot.work_dirs.begin(),
                                   snapshot.work_dirs.end());
  const std::set<std::string> live(snapshot.live_units.begin(),
                                   snapshot.live_units.end());

  for (const ServiceRecord& record : snapshot.registry) {
    const std::string& name = record.service_name;
    const bool has_unit = units.count(name) != 0;
    const bool has_work = work.count(name) != 0;
    if (!has_unit) {
      add_finding(&report, RepairIssue::kRegisteredUnitMissing, name,
                  "registered, but no unit file exists for it",
                  "berthpanel __deploy " + name +
                      "  (recreate the unit from its recipe)");
    } else if (live.count(name) == 0) {
      std::string repair = "systemctl --user start ";
      repair += name;
      repair += ".service  (or: berthpanel __deploy ";
      repair += name;
      repair += ")";
      add_finding(&report, RepairIssue::kRegisteredUnitInactive, name,
                  "registered and the unit file exists, but the unit is not "
                  "active",
                  std::move(repair));
    }
    if (has_unit && enabled.count(name) == 0) {
      add_finding(&report, RepairIssue::kRegisteredNotEnabled, name,
                  "registered unit is not enabled: it will not start on boot",
                  "systemctl --user enable " + name + ".service");
    }
    if (!has_work) {
      add_finding(&report, RepairIssue::kRegisteredWorkDirMissing, name,
                  "registered, but its work dir is missing under the services "
                  "layout",
                  "berthpanel __deploy " + name +
                      "  (or stop using the service)");
    }
  }

  for (const std::string& unit : snapshot.unit_files) {
    if (registry_names.count(unit) != 0)
      continue;
    if (live.count(unit) != 0) {
      add_finding(&report, RepairIssue::kUnitLiveUnregistered, unit,
                  "the unit is ACTIVE but the service is not in the registry",
                  "systemctl --user disable --now " + unit +
                      ".service  (then remove the orphan unit or redeploy it)");
    } else {
      add_finding(&report, RepairIssue::kUnitUnregistered, unit,
                  "a unit file exists but the service is not in the registry",
                  "berthpanel __deploy " + unit +
                      "  (or remove the orphan unit)");
    }
  }

  for (const std::string& link : snapshot.enabled_units) {
    if (units.count(link) != 0)
      continue;
    add_finding(&report, RepairIssue::kEnableLinkDangling, link,
                "an enable symlink exists but its unit file is missing",
                "rm the default.target.wants/" + link +
                    ".service symlink; systemctl --user daemon-reload");
  }

  for (const std::string& dir : snapshot.work_dirs) {
    if (registry_names.count(dir) != 0 || journal_names.count(dir) != 0)
      continue;
    add_finding(&report, RepairIssue::kWorkDirOrphan, dir,
                "work dir has no registry or journal owner; it may hold user "
                "data",
                "inspect it under the services layout; remove it manually only "
                "if the data is unwanted");
  }

  if (snapshot.lock_busy) {
    add_finding(&report, RepairIssue::kDeployInProgress, "",
                "a deploy holds deploy.lock; journal entries were not judged",
                "wait for the running deploy to finish");
  } else {
    for (const JournalEntry& entry : snapshot.journal) {
      if (journal_unsafe.count(entry.service_name) != 0) {
        add_finding(&report, RepairIssue::kJournalUnsafeWorkDir,
                    entry.service_name,
                    "journal work_dir '" + entry.work_dir +
                        "' is outside the services layout or is not this "
                        "service's",
                    "inspect the journal entry manually; __cleanup refuses it",
                    entry.stack);
        continue;
      }
      if (registry_names.count(entry.service_name) != 0) {
        add_finding(&report, RepairIssue::kJournalStaleRegistered,
                    entry.service_name,
                    "journal entry exists for a REGISTERED service (the "
                    "write-back was lost)",
                    "berthpanel __cleanup  (drops the stale entry, deletes "
                    "nothing)",
                    entry.stack);
        continue;
      }
      add_finding(&report, RepairIssue::kJournalInterrupted,
                  entry.service_name,
                  "interrupted deploy: journal entry present and the deploy "
                  "lock is free (stage '" +
                      entry.stage + "')",
                  "berthpanel __cleanup", entry.stack);
    }
  }

  std::sort(report.findings.begin(), report.findings.end(),
            [](const RepairFinding& a, const RepairFinding& b) {
              if (a.service_name != b.service_name)
                return a.service_name < b.service_name;
              return static_cast<int>(a.issue) < static_cast<int>(b.issue);
            });
  return report;
}

std::string render_repair_report(const RepairReport& report) {
  if (report.findings.empty())
    return "repair: no discrepancies\n";
  std::string out = "repair: " + std::to_string(report.findings.size()) +
                    " finding(s)\n";
  for (const RepairFinding& finding : report.findings) {
    out += "  ";
    if (!finding.service_name.empty())
      out += finding.service_name + ": ";
    out += finding.detail;
    if (!finding.stack.empty())
      out += " [stack " + finding.stack + "]";
    out += "\n    fix: " + finding.repair + "\n";
  }
  return out;
}

nlohmann::json repair_json(const RepairReport& report) {
  nlohmann::json doc = json_document("repair");
  nlohmann::json array = nlohmann::json::array();
  for (const RepairFinding& finding : report.findings) {
    nlohmann::json item{{"issue", issue_name(finding.issue)},
                        {"detail", finding.detail},
                        {"repair", finding.repair}};
    if (!finding.service_name.empty())
      item["service"] = finding.service_name;
    if (!finding.stack.empty())
      item["stack"] = finding.stack;
    array.push_back(std::move(item));
  }
  doc["findings"] = std::move(array);
  return doc;
}

}
