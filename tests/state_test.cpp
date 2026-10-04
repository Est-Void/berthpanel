// Tests for state.h (stage 2.4): the full register/list/get/unregister
// cycle across process-boundary-free reloads, upsert semantics, loud
// rejection of invalid names, corrupt-file quarantine, 0600 storage.
// Paths are constructed by hand — no environment is touched here.

#include "state.h"

#include <filesystem>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.state_json = p.state_dir / "state.json";
  return p;
}

berth::ServiceRecord memos() {
  berth::ServiceRecord r;
  r.service_name = "berthpanel-memos";
  r.display_name = "Memos";
  r.description = "Note taking";
  r.repo = "usememos/memos";
  r.commit = "abc123";
  r.port = 8234;
  return r;
}

}  // namespace

TEST(state_fresh_install_is_empty_not_error) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto listed = berth::registry_list(p);
  CHECK(listed.is_ok());
  CHECK(listed.value().empty());
}

TEST(state_full_cycle) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  CHECK(berth::registry_register(p, memos()).is_ok());

  const auto got = berth::registry_get(p, "berthpanel-memos");
  CHECK(got.is_ok());
  CHECK(got.value().has_value());
  CHECK(got.value()->port == 8234);
  CHECK(got.value()->commit == "abc123");
  CHECK(got.value()->repo == "usememos/memos");

  const auto listed = berth::registry_list(p);
  CHECK(listed.is_ok());
  CHECK(listed.value().size() == 1);

  CHECK(berth::registry_unregister(p, "berthpanel-memos").is_ok());
  const auto gone = berth::registry_get(p, "berthpanel-memos");
  CHECK(gone.is_ok());
  CHECK(!gone.value().has_value());
}

TEST(state_register_is_upsert) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  CHECK(berth::registry_register(p, memos()).is_ok());
  berth::ServiceRecord updated = memos();
  updated.port = 9999;
  updated.commit = "def456";
  CHECK(berth::registry_register(p, updated).is_ok());

  const auto listed = berth::registry_list(p);
  CHECK(listed.is_ok());
  CHECK(listed.value().size() == 1);
  CHECK(listed.value()[0].port == 9999);
  CHECK(listed.value()[0].commit == "def456");
}

TEST(state_rejects_invalid_names) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  berth::ServiceRecord r = memos();
  r.service_name = "memos";  // missing prefix
  CHECK(berth::registry_register(p, r).is_err());
  r.service_name = "berthpanel-";  // empty stem
  CHECK(berth::registry_register(p, r).is_err());
  r.service_name = "berthpanel-a/b";  // path separator
  CHECK(berth::registry_register(p, r).is_err());
  r.service_name = "berthpanel-a b";  // space
  CHECK(berth::registry_register(p, r).is_err());
  r.service_name = std::string("berthpanel-") + std::string(200, 'x');
  CHECK(berth::registry_register(p, r).is_err());
}

TEST(state_unregister_unknown_is_error) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto r = berth::registry_unregister(p, "berthpanel-ghost");
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("unknown") != std::string::npos);
}

TEST(state_corrupt_file_is_loud_and_backed_up) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, memos()).is_ok());

  // Corrupt the registry on disk, as a crash mid-write or a hand edit would.
  CHECK(
      berth::atomic_write(p.state_json, "{oops", berth::kPermPrivate).is_ok());

  const auto r = berth::registry_get(p, "berthpanel-memos");
  CHECK(r.is_err());  // never reads as empty (§11)
  CHECK(r.error().ctx().find("parse") != std::string::npos);
  CHECK(std::filesystem::exists(p.state_json.string() + ".corrupt"));
}

TEST(state_corrupt_schema_is_loud) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  // Parses fine, but is not a registry.
  CHECK(berth::atomic_write(p.state_json, "{\"hello\": true}",
                            berth::kPermPrivate)
            .is_ok());
  const auto r = berth::registry_list(p);
  CHECK(r.is_err());
  CHECK(std::filesystem::exists(p.state_json.string() + ".corrupt"));
}

TEST(state_bad_port_in_file_is_rejected) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  const std::string bad =
      R"({"version": 1, "services": [{"service_name": "berthpanel-memos",)"
      R"("display_name": "Memos", "description": "", "repo": "r",)"
      R"("commit": "c", "port": 70000}]})";
  CHECK(berth::atomic_write(p.state_json, bad, berth::kPermPrivate).is_ok());
  const auto r = berth::registry_list(p);
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("port out of range") != std::string::npos);
}

TEST(state_file_is_private_and_temp_free) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, memos()).is_ok());
  CHECK(std::filesystem::status(p.state_json).permissions() ==
        berth::kPermPrivate);
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(p.state_dir)) {
    (void)entry;
    ++files;
  }
  CHECK(files == 1);
}

TEST(validate_unit_name_rules) {
  CHECK(berth::validate_unit_name("berthpanel-memos"));
  CHECK(berth::validate_unit_name("berthpanel-a_1.b@c-d"));
  CHECK(!berth::validate_unit_name("memos"));
  CHECK(!berth::validate_unit_name("berthpanel-"));
  CHECK(!berth::validate_unit_name(""));
  CHECK(!berth::validate_unit_name("berthpanel-a/b"));
  CHECK(!berth::validate_unit_name("berthpanel-a b"));
  CHECK(!berth::validate_unit_name(std::string("berthpanel-") +
                                   std::string(200, 'x')));
}
