// Tests for journal.h (stage 4.4): the interrupted-deploy record.
// Round-trip, upsert-by-service, idempotent removal, and the §11 rule
// that a corrupt journal is quarantined loudly, never read as empty.

#include "berth/journal.h"

#include <filesystem>
#include <string>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::journal_load;
using berth::journal_remove;
using berth::journal_upsert;
using berth::JournalEntry;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.deploy_journal = p.state_dir / "deploy-journal.json";
  return p;
}

JournalEntry memos_entry(const std::string& stage) {
  JournalEntry e;
  e.service_name = "berthpanel-memos";
  e.work_dir = "/home/u/.local/share/berthpanel/services/berthpanel-memos";
  e.repo = "usememos/memos";
  e.commit = "0123456789abcdef0123456789abcdef01234567";
  e.stage = stage;
  e.started_at_unix = 1750000000;
  return e;
}

}
TEST(journal_missing_file_is_empty_not_error) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto loaded = journal_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().empty());
}

TEST(journal_upsert_round_trip) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(journal_upsert(p, memos_entry("cloning")).is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 1);
  const JournalEntry& e = loaded.value().front();
  CHECK(e.service_name == "berthpanel-memos");
  CHECK(e.stage == "cloning");
  CHECK(e.repo == "usememos/memos");
  CHECK(e.commit == "0123456789abcdef0123456789abcdef01234567");
  CHECK(e.started_at_unix == 1750000000);
}

TEST(journal_upsert_replaces_same_service) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(journal_upsert(p, memos_entry("cloning")).is_ok());
  CHECK(journal_upsert(p, memos_entry("building")).is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 1);
  CHECK(loaded.value().front().stage == "building");
}

TEST(journal_remove_is_idempotent) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(journal_upsert(p, memos_entry("building")).is_ok());
  CHECK(journal_remove(p, "berthpanel-memos").is_ok());
  CHECK(journal_remove(p, "berthpanel-memos").is_ok());  // second run is fine
  const auto loaded = journal_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().empty());
}

TEST(journal_rejects_invalid_entries_before_write) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  JournalEntry bad = memos_entry("building");
  bad.service_name = "memos";  // missing berthpanel- prefix
  CHECK(journal_upsert(p, bad).is_err());
}

TEST(journal_corrupt_is_loud_and_backed_up) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  CHECK(berth::atomic_write(p.deploy_journal, "{broken", berth::kPermPrivate)
            .is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_err());
  CHECK(std::filesystem::exists(p.deploy_journal.string() + ".corrupt"));
}

TEST(journal_rejects_unknown_stage_in_file) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  const std::string body = R"({"version": 1, "entries": [{"service_name":
    "berthpanel-memos", "work_dir": "/tmp/w", "repo": "o/r", "commit": "c",
    "stage": "teleporting", "started_at_unix": 1}]})";
  CHECK(
      berth::atomic_write(p.deploy_journal, body, berth::kPermPrivate).is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_err());
  CHECK(loaded.error().ctx().find("stage") != std::string::npos);
}

TEST(journal_file_is_private) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(journal_upsert(p, memos_entry("building")).is_ok());
  CHECK(std::filesystem::status(p.deploy_journal).permissions() ==
        berth::kPermPrivate);
}

TEST(journal_stack_tag_round_trips) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  JournalEntry e = memos_entry("building");
  e.stack = "berthpanel-group";
  CHECK(journal_upsert(p, e).is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().front().stack == "berthpanel-group");
}

TEST(journal_upsert_many_writes_the_whole_group_at_once) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  JournalEntry db = memos_entry("cloning");
  db.service_name = "berthpanel-group-db";
  db.stack = "berthpanel-group";
  JournalEntry app = memos_entry("cloning");
  app.service_name = "berthpanel-group-app";
  app.stack = "berthpanel-group";
  CHECK(berth::journal_upsert_many(p, {db, app}).is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 2);

  // A second batch replaces only the matching key, leaving the sibling.
  app.stage = "building";
  CHECK(berth::journal_upsert_many(p, {app}).is_ok());
  const auto after = journal_load(p);
  CHECK(after.is_ok());
  CHECK(after.value().size() == 2);
  for (const JournalEntry& e : after.value()) {
    if (e.service_name == "berthpanel-group-app")
      CHECK(e.stage == "building");
    else
      CHECK(e.stage == "cloning");
  }
}

TEST(journal_rejects_an_invalid_stack_tag_in_file) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  const std::string body = R"({"version": 1, "entries": [{"service_name":
    "berthpanel-group-db", "work_dir": "/tmp/w", "repo": "o/r", "commit": "c",
    "stage": "cloning", "started_at_unix": 1, "stack": "no-prefix"}]})";
  CHECK(
      berth::atomic_write(p.deploy_journal, body, berth::kPermPrivate).is_ok());

  const auto loaded = journal_load(p);
  CHECK(loaded.is_err());
  CHECK(loaded.error().ctx().find("stack") != std::string::npos);
}
