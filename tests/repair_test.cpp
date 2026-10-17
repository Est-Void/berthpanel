#include "berth/repair.h"

#include <filesystem>
#include <string>
#include <vector>

#include "berth/journal.h"
#include "berth/lock.h"
#include "core/atomic.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::JournalEntry;
using berth::RepairIssue;
using berth::RepairReport;
using berth::RepairSnapshot;
using berth::Result;
using berth::ServiceRecord;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.secrets_dir = p.state_dir / "secrets";
  p.services_dir = root / "share" / "berthpanel" / "services";
  p.units_dir = root / "config" / "systemd" / "user";
  p.state_json = p.state_dir / "state.json";
  p.deploy_journal = p.state_dir / "deploy-journal.json";
  p.deploy_lock = p.state_dir / "deploy.lock";
  p.deploy_log = p.state_dir / "deploy.log";
  return p;
}

ServiceRecord record(const std::string& service_name) {
  ServiceRecord r;
  r.service_name = service_name;
  r.display_name = service_name;
  r.repo = "owner/repo";
  r.commit = std::string(40, 'a');
  r.port = 8234;
  return r;
}

JournalEntry entry(const berth::Paths& p, const std::string& service_name,
                   const std::string& stage) {
  JournalEntry e;
  e.service_name = service_name;
  e.work_dir = (p.services_dir / service_name).string();
  e.repo = "owner/repo";
  e.commit = std::string(40, 'a');
  e.stage = stage;
  e.started_at_unix = 1;
  return e;
}

bool has_issue(const RepairReport& report, const RepairIssue issue,
               const std::string& service_name) {
  for (const berth::RepairFinding& finding : report.findings) {
    if (finding.issue == issue && finding.service_name == service_name)
      return true;
  }
  return false;
}

bool contains(const std::vector<std::string>& values,
              const std::string& needle) {
  for (const std::string& value : values) {
    if (value == needle)
      return true;
  }
  return false;
}

}

TEST(repair_detects_a_live_unit_missing_from_the_registry_and_the_reverse) {
  RepairSnapshot snapshot;
  snapshot.registry = {record("berthpanel-registered")};
  snapshot.unit_files = {"berthpanel-stray"};
  snapshot.live_units = {"berthpanel-stray"};
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(has_issue(report, RepairIssue::kUnitLiveUnregistered,
                  "berthpanel-stray"));
  CHECK(has_issue(report, RepairIssue::kRegisteredUnitMissing,
                  "berthpanel-registered"));
  CHECK(!has_issue(report, RepairIssue::kRegisteredUnitInactive,
                   "berthpanel-registered"));
}

TEST(repair_flags_registered_but_inactive_unenabled_and_without_a_work_dir) {
  RepairSnapshot snapshot;
  snapshot.registry = {record("berthpanel-x")};
  snapshot.unit_files = {"berthpanel-x"};
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(has_issue(report, RepairIssue::kRegisteredUnitInactive,
                  "berthpanel-x"));
  CHECK(has_issue(report, RepairIssue::kRegisteredNotEnabled,
                  "berthpanel-x"));
  CHECK(has_issue(report, RepairIssue::kRegisteredWorkDirMissing,
                  "berthpanel-x"));
}

TEST(repair_flags_an_unregistered_unit_file_but_not_as_live) {
  RepairSnapshot snapshot;
  snapshot.unit_files = {"berthpanel-old"};
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(has_issue(report, RepairIssue::kUnitUnregistered, "berthpanel-old"));
  CHECK(!has_issue(report, RepairIssue::kUnitLiveUnregistered,
                   "berthpanel-old"));
}

TEST(repair_flags_journal_states_and_carries_the_stack_tag) {
  RepairSnapshot snapshot;
  snapshot.registry = {record("berthpanel-live")};
  snapshot.unit_files = {"berthpanel-live"};
  snapshot.work_dirs = {"berthpanel-live"};
  snapshot.live_units = {"berthpanel-live"};
  snapshot.enabled_units = {"berthpanel-live"};
  JournalEntry stale = entry(test_paths("/tmp/repair-unused"),
                             "berthpanel-live", "registering");
  stale.stack = "berthpanel-group";
  snapshot.journal = {stale,
                      entry(test_paths("/tmp/repair-unused"),
                            "berthpanel-ghost", "building"),
                      entry(test_paths("/tmp/repair-unused"),
                            "berthpanel-evil", "cloning")};
  snapshot.journal_unsafe = {"berthpanel-evil"};
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(has_issue(report, RepairIssue::kJournalStaleRegistered,
                  "berthpanel-live"));
  CHECK(has_issue(report, RepairIssue::kJournalInterrupted,
                  "berthpanel-ghost"));
  CHECK(has_issue(report, RepairIssue::kJournalUnsafeWorkDir,
                  "berthpanel-evil"));
  for (const berth::RepairFinding& finding : report.findings) {
    if (finding.issue == RepairIssue::kJournalStaleRegistered)
      CHECK(finding.stack == "berthpanel-group");
  }
}

TEST(repair_does_not_judge_the_journal_while_a_deploy_holds_the_lock) {
  RepairSnapshot snapshot;
  snapshot.journal = {entry(test_paths("/tmp/repair-unused"),
                            "berthpanel-x", "building")};
  snapshot.lock_busy = true;
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(has_issue(report, RepairIssue::kDeployInProgress, ""));
  CHECK(!has_issue(report, RepairIssue::kJournalInterrupted, "berthpanel-x"));
}

TEST(repair_flags_orphan_work_dirs_and_dangling_enable_links) {
  RepairSnapshot snapshot;
  snapshot.registry = {record("berthpanel-owned")};
  snapshot.unit_files = {"berthpanel-owned"};
  snapshot.live_units = {"berthpanel-owned"};
  snapshot.enabled_units = {"berthpanel-owned", "berthpanel-dangling"};
  snapshot.work_dirs = {"berthpanel-owned", "berthpanel-free"};
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(has_issue(report, RepairIssue::kWorkDirOrphan, "berthpanel-free"));
  CHECK(!has_issue(report, RepairIssue::kWorkDirOrphan, "berthpanel-owned"));
  CHECK(has_issue(report, RepairIssue::kEnableLinkDangling,
                  "berthpanel-dangling"));
}

TEST(repair_a_consistent_snapshot_is_quiet) {
  RepairSnapshot snapshot;
  snapshot.registry = {record("berthpanel-ok")};
  snapshot.unit_files = {"berthpanel-ok"};
  snapshot.live_units = {"berthpanel-ok"};
  snapshot.enabled_units = {"berthpanel-ok"};
  snapshot.work_dirs = {"berthpanel-ok"};
  const RepairReport report = berth::repair_check(snapshot);
  CHECK(report.findings.empty());
  CHECK(berth::render_repair_report(report).find("no discrepancies") !=
        std::string::npos);
}

TEST(repair_json_is_the_versioned_envelope) {
  RepairSnapshot snapshot;
  snapshot.unit_files = {"berthpanel-stray"};
  snapshot.live_units = {"berthpanel-stray"};
  const nlohmann::json doc =
      berth::repair_json(berth::repair_check(snapshot));
  CHECK(doc.at("command") == "repair");
  CHECK(doc.at("schema") == 1);
  CHECK(doc.at("findings").is_array());
  CHECK(doc.at("findings").front().at("issue") == "unit-live-unregistered");
}

TEST(repair_collector_reads_fixtures_and_injects_liveness) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, record("berthpanel-reg")).is_ok());
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(berth::atomic_write(p.units_dir / "berthpanel-reg.service", "[Unit]\n")
            .is_ok());
  CHECK(berth::atomic_write(p.units_dir / "berthpanel-stray.service",
                            "[Unit]\n")
            .is_ok());
  CHECK(berth::ensure_dir(p.units_dir / "default.target.wants").is_ok());
  std::error_code ec;
  std::filesystem::create_symlink(
      p.units_dir / "berthpanel-reg.service",
      p.units_dir / "default.target.wants" / "berthpanel-reg.service", ec);
  CHECK(!ec);
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-reg").is_ok());
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-orphan").is_ok());
  CHECK(berth::journal_upsert(p, entry(p, "berthpanel-ghost", "building"))
            .is_ok());
  JournalEntry evil = entry(p, "berthpanel-evil", "cloning");
  evil.work_dir = "/etc";
  CHECK(berth::journal_upsert(p, evil).is_ok());

  const auto snapshot = berth::collect_repair_snapshot(
      p, [](const std::string& name) -> Result<bool> {
        return name == "berthpanel-stray";
      });
  CHECK(snapshot.is_ok());
  CHECK(contains(snapshot.value().unit_files, "berthpanel-reg"));
  CHECK(contains(snapshot.value().unit_files, "berthpanel-stray"));
  CHECK(contains(snapshot.value().enabled_units, "berthpanel-reg"));
  CHECK(contains(snapshot.value().work_dirs, "berthpanel-reg"));
  CHECK(contains(snapshot.value().work_dirs, "berthpanel-orphan"));
  CHECK(contains(snapshot.value().live_units, "berthpanel-stray"));
  CHECK(!contains(snapshot.value().live_units, "berthpanel-reg"));
  CHECK(contains(snapshot.value().journal_unsafe, "berthpanel-evil"));
  CHECK(!contains(snapshot.value().journal_unsafe, "berthpanel-ghost"));
  CHECK(!snapshot.value().lock_busy);

  const RepairReport report = berth::repair_check(snapshot.value());
  CHECK(has_issue(report, RepairIssue::kUnitLiveUnregistered,
                  "berthpanel-stray"));
  CHECK(has_issue(report, RepairIssue::kRegisteredUnitInactive,
                  "berthpanel-reg"));
  CHECK(has_issue(report, RepairIssue::kWorkDirOrphan, "berthpanel-orphan"));
  CHECK(has_issue(report, RepairIssue::kJournalInterrupted,
                  "berthpanel-ghost"));
  CHECK(has_issue(report, RepairIssue::kJournalUnsafeWorkDir,
                  "berthpanel-evil"));
}

TEST(repair_collector_sees_a_held_deploy_lock) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto held = berth::DeployLock::acquire(p);
  CHECK(held.is_ok());
  CHECK(berth::journal_upsert(p, entry(p, "berthpanel-x", "building")).is_ok());

  const auto snapshot = berth::collect_repair_snapshot(
      p, [](const std::string&) -> Result<bool> { return false; });
  CHECK(snapshot.is_ok());
  CHECK(snapshot.value().lock_busy);
  const RepairReport report = berth::repair_check(snapshot.value());
  CHECK(has_issue(report, RepairIssue::kDeployInProgress, ""));
  CHECK(!has_issue(report, RepairIssue::kJournalInterrupted, "berthpanel-x"));
}
