#include "berth/manifest.h"

#include <filesystem>
#include <string>

#include "berth/recipes.h"
#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::Recipe;

namespace {

constexpr const char* kCommit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";

nlohmann::json good_manifest() {
  return {
      {"owner", "octocat"},
      {"name", "hello"},
      {"commit", kCommit},
      {"language", "python"},
      {"service_name", "berthpanel-hello"},
      {"display_name", "Hello"},
      {"description", "test"},
      {"binary_name", "hello"},
      {"start_cmd", "python3 -m http.server {port} --bind 127.0.0.1"},
      {"build_steps", {{"python3", "-c", "pass"}}},
      {"tools", {"python3"}},
      {"port", 0},
      {"health_path", "/"},
      {"res", {{"memory_max", "128M"}, {"tasks_max", 64}}},
  };
}

std::filesystem::path write_json(const TempDir& tmp, const std::string& name,
                                 const std::string& body) {
  const std::filesystem::path path = tmp.path() / name;
  const auto wrote = berth::atomic_write(path, body, berth::kPermShared);
  CHECK(wrote.is_ok());
  return path;
}

std::filesystem::path write_manifest(const TempDir& tmp, const std::string& name,
                                     const nlohmann::json& doc) {
  return write_json(tmp, name, doc.dump(2) + "\n");
}

bool has_corrupt_sibling(const std::filesystem::path& path) {
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().find(".corrupt") !=
        std::string::npos) {
      return true;
    }
  }
  return false;
}

}
TEST(manifest_loads_and_renders_the_plan) {
  const TempDir tmp;
  const std::filesystem::path path =
      write_manifest(tmp, "manifest.json", good_manifest());
  const auto loaded = berth::load_manifest(path);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().service_name == "berthpanel-hello");

  const std::string plan = berth::render_manifest_plan(loaded.value());
  CHECK(plan.find("berthpanel-hello") != std::string::npos);
  CHECK(plan.find("octocat/hello@2b2192d") != std::string::npos);
}

TEST(manifest_rejects_invalid_egress) {
  const TempDir tmp;
  nlohmann::json doc = good_manifest();
  doc["egress"] = "open";
  const std::filesystem::path path =
      write_manifest(tmp, "manifest.json", doc);
  const auto loaded = berth::load_manifest(path);
  CHECK(loaded.is_err());
  CHECK(loaded.error().ctx().find("deny-all") != std::string::npos);
}

TEST(manifest_rejects_missing_or_zero_limits) {
  const TempDir tmp;
  nlohmann::json missing = good_manifest();
  missing.erase("res");
  CHECK(berth::load_manifest(write_manifest(tmp, "missing.json", missing))
            .is_err());

  nlohmann::json zero = good_manifest();
  zero["res"]["tasks_max"] = 0;
  CHECK(
      berth::load_manifest(write_manifest(tmp, "zero.json", zero)).is_err());
}

TEST(manifest_rejects_malformed_json_without_touching_the_file) {
  const TempDir tmp;
  const std::filesystem::path path =
      write_json(tmp, "broken.json", "{not json");
  const std::string before = berth::read_file(path).value();

  const auto loaded = berth::load_manifest(path);
  CHECK(loaded.is_err());
  CHECK(loaded.error().ctx().find("broken.json") != std::string::npos);

  CHECK(std::filesystem::exists(path));
  CHECK(berth::read_file(path).value() == before);
  CHECK(!has_corrupt_sibling(path));
}

TEST(manifest_json_carries_the_document_and_round_trips) {
  const TempDir tmp;
  const std::filesystem::path path =
      write_manifest(tmp, "manifest.json", good_manifest());
  const auto loaded = berth::load_manifest(path);
  CHECK(loaded.is_ok());

  const nlohmann::json doc = berth::manifest_json(loaded.value());
  CHECK(doc.at("schema") == 1);
  CHECK(doc.at("command") == "check-manifest");
  CHECK(doc.contains("recipe"));

  const berth::Result<Recipe> back =
      berth::recipe_from_json(doc.at("recipe"));
  CHECK(back.is_ok());
  CHECK(back.value().owner == "octocat");
  CHECK(back.value().commit == kCommit);
}
