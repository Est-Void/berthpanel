// Tests for json.h (stage 2.3): store/load round-trips, 0600 store, corrupt
// files quarantined loudly (never silent defaults, §11), progressive
// backup naming, missing files are a normal error.

#include "core/json.h"

#include <filesystem>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

TEST(json_store_load_round_trip) {
  const TempDir tmp;
  const auto path = tmp.path() / "doc.json";
  const nlohmann::json doc = {{"name", "memos"},
                              {"port", 8234},
                              {"tags", nlohmann::json::array({"a", "b"})}};

  CHECK(berth::json_store(path, doc).is_ok());
  const auto r = berth::json_load(path);
  CHECK(r.is_ok());
  CHECK(r.value() == doc);
}

TEST(json_store_is_private_and_pretty) {
  const TempDir tmp;
  const auto path = tmp.path() / "doc.json";
  CHECK(berth::json_store(path, nlohmann::json{{"k", "v"}}).is_ok());
  CHECK(std::filesystem::status(path).permissions() == berth::kPermPrivate);
  const auto raw = berth::read_file(path);
  CHECK(raw.is_ok());
  CHECK(raw.value().find("  \"k\": \"v\"") != std::string::npos);
}

TEST(json_store_updates_existing) {
  const TempDir tmp;
  const auto path = tmp.path() / "doc.json";
  CHECK(berth::json_store(path, nlohmann::json{{"gen", 1}}).is_ok());
  CHECK(berth::json_store(path, nlohmann::json{{"gen", 2}}).is_ok());
  const auto r = berth::json_load(path);
  CHECK(r.is_ok());
  CHECK(r.value()["gen"] == 2);
}

TEST(json_load_missing_file_is_error) {
  const TempDir tmp;
  const auto r = berth::json_load(tmp.path() / "absent.json");
  CHECK(r.is_err());
  // No quarantine for a missing file — nothing to back up.
  CHECK(!std::filesystem::exists(tmp.path() / "absent.json.corrupt"));
}

TEST(json_load_quarantines_corrupt_file) {
  const TempDir tmp;
  const auto path = tmp.path() / "doc.json";
  CHECK(berth::atomic_write(path, "{not json", berth::kPermPrivate).is_ok());

  const auto r = berth::json_load(path);
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("parse") != std::string::npos);

  // Loud backup, original left in place.
  const auto backup = tmp.path() / "doc.json.corrupt";
  CHECK(std::filesystem::exists(backup));
  const auto raw = berth::read_file(backup);
  CHECK(raw.is_ok());
  CHECK(raw.value() == "{not json");
  CHECK(std::filesystem::exists(path));
}

TEST(json_quarantine_never_overwrites_previous_backup) {
  const TempDir tmp;
  const auto path = tmp.path() / "doc.json";
  CHECK(berth::atomic_write(path, "bad one", berth::kPermPrivate).is_ok());
  CHECK(berth::json_load(path).is_err());  // creates .corrupt

  CHECK(berth::atomic_write(path, "bad two", berth::kPermPrivate).is_ok());
  CHECK(berth::json_load(path).is_err());  // creates .corrupt.1

  const auto first = berth::read_file(tmp.path() / "doc.json.corrupt");
  CHECK(first.is_ok());
  CHECK(first.value() == "bad one");
  const auto second = berth::read_file(tmp.path() / "doc.json.corrupt.1");
  CHECK(second.is_ok());
  CHECK(second.value() == "bad two");
}

TEST(json_load_accepts_pretty_files) {
  const TempDir tmp;
  const auto path = tmp.path() / "doc.json";
  const std::string pretty =
      "{\n  \"a\": 1,\n  \"b\": [\n    2,\n    3\n  ]\n}\n";
  CHECK(berth::atomic_write(path, pretty, berth::kPermPrivate).is_ok());
  const auto r = berth::json_load(path);
  CHECK(r.is_ok());
  CHECK(r.value()["b"].size() == 2);
}
