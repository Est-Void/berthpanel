// Tests for userrecipes.h (stage 4.19): storage of BYO recipes under
// paths.recipes_dir, the shared validator at store AND load time, the
// forced berthpanel- prefix, loud corruption handling, and the
// user-over-builtin resolution in catalog.cpp.

#include "berth/userrecipes.h"

#include <filesystem>
#include <string>

#include "berth/catalog.h"
#include "berth/recipes.h"
#include "core/atomic.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::Recipe;
using berth::Result;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.config_dir = root / "config" / "berthpanel";
  p.recipes_dir = p.config_dir / "recipes";
  p.services_dir = root / "share" / "berthpanel" / "services";
  p.units_dir = root / "config" / "systemd" / "user";
  p.state_json = p.state_dir / "state.json";
  p.deploy_journal = p.state_dir / "deploy-journal.json";
  p.deploy_lock = p.state_dir / "deploy.lock";
  p.deploy_log = p.state_dir / "deploy.log";
  p.net_log = p.state_dir / "net.log";
  return p;
}

Recipe go_recipe(const std::string& service_name = "berthpanel-myapp") {
  Recipe r;
  r.owner = "someone";
  r.name = "myapp";
  r.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  r.language = "go";
  r.service_name = service_name;
  r.display_name = "My App";
  r.description = "BYO app";
  r.build_steps = {{"go", "build", "-o", "{project}/myapp", "."}};
  r.binary_name = "myapp";
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port}";
  r.tools = {"go"};
  r.res = {"384M", 200};
  return r;
}

}  // namespace

TEST(user_recipe_store_and_load_roundtrip) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto stored = berth::user_recipe_store(p, go_recipe());
  CHECK(stored.is_ok());
  CHECK(stored.value() == p.recipes_dir / "berthpanel-myapp.json");

  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 1);
  CHECK(loaded.value().front().service_name == "berthpanel-myapp");
  CHECK(loaded.value().front().owner_repo() == "someone/myapp");
  CHECK(loaded.value().front().res.memory_max == "384M");
}

TEST(user_recipe_store_forces_the_berthpanel_prefix) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  Recipe bare = go_recipe("myapp");  // no prefix
  const auto stored = berth::user_recipe_store(p, bare);
  CHECK(stored.is_ok());
  CHECK(stored.value().filename() == "berthpanel-myapp.json");

  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().front().service_name == "berthpanel-myapp");
}

TEST(user_recipe_store_rejects_a_recipe_that_fails_validation) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  Recipe bad = go_recipe();
  bad.res = {"", 0};  // §8: limits are mandatory
  const auto stored = berth::user_recipe_store(p, bad);
  CHECK(stored.is_err());
  CHECK(stored.error().ctx().find("memory_max") != std::string::npos);
  // Nothing was written: a rejected recipe never reaches the directory.
  CHECK(!std::filesystem::exists(p.recipes_dir));
}

TEST(user_recipe_store_rejects_a_wildcard_bind_loudly) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  Recipe bad = go_recipe();
  bad.start_cmd = "{self} --addr :{port}";
  const auto stored = berth::user_recipe_store(p, bad);
  CHECK(stored.is_err());
}

TEST(user_recipe_store_upserts_by_service_name) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  Recipe first = go_recipe();
  first.res = {"384M", 200};
  CHECK(berth::user_recipe_store(p, first).is_ok());
  Recipe second = go_recipe();
  second.res = {"768M", 400};
  const auto replaced = berth::user_recipe_store(p, second);
  CHECK(replaced.is_ok());

  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 1);
  CHECK(loaded.value().front().res.memory_max == "768M");
}

TEST(user_recipes_load_fails_loudly_on_a_corrupt_file) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::user_recipe_store(p, go_recipe()).is_ok());
  Recipe other = go_recipe("berthpanel-second");
  CHECK(berth::user_recipe_store(p, other).is_ok());
  CHECK(
      berth::atomic_write(p.recipes_dir / "berthpanel-broken.json", "{not json")
          .is_ok());

  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_err());
  // The error names the offending file (§11: loud, never silent skip).
  CHECK(loaded.error().ctx().find("berthpanel-broken.json") !=
        std::string::npos);
}

TEST(user_recipes_load_fails_on_a_file_that_fails_validation) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.recipes_dir).is_ok());
  // Valid JSON, invalid recipe: no limits, no loopback bind.
  const std::string body =
      "{\"owner\":\"someone\",\"name\":\"myapp\","
      "\"commit\":\"2b2192d4e153bd04f1d325b60fd880cf00d68b01\","
      "\"language\":\"go\",\"service_name\":\"berthpanel-bad\","
      "\"display_name\":\"Bad\",\"binary_name\":\"myapp\","
      "\"start_cmd\":\"{self} --port {port}\","
      "\"tools\":[\"go\"]}";
  CHECK(
      berth::atomic_write(p.recipes_dir / "berthpanel-bad.json", body).is_ok());

  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_err());
  CHECK(loaded.error().ctx().find("berthpanel-bad.json") != std::string::npos);
}

TEST(user_recipes_load_ignores_non_json_files) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::user_recipe_store(p, go_recipe()).is_ok());
  CHECK(berth::atomic_write(p.recipes_dir / "README", "not a recipe").is_ok());

  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 1);
}

TEST(user_recipes_load_with_no_directory_is_empty_not_an_error) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().empty());
}

TEST(user_recipe_remove_is_idempotent_and_validates_the_name) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::user_recipe_store(p, go_recipe()).is_ok());

  const auto removed = berth::user_recipe_remove(p, "berthpanel-myapp");
  CHECK(removed.is_ok());
  CHECK(removed.value());
  CHECK(!std::filesystem::exists(p.recipes_dir / "berthpanel-myapp.json"));

  const auto again = berth::user_recipe_remove(p, "berthpanel-myapp");
  CHECK(again.is_ok());
  CHECK(!again.value());

  CHECK(berth::user_recipe_remove(p, "no-prefix").is_err());
}

TEST(user_recipe_get_finds_one_recipe_or_nothing) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::user_recipe_store(p, go_recipe()).is_ok());

  const auto found = berth::user_recipe_get(p, "berthpanel-myapp");
  CHECK(found.is_ok());
  CHECK(found.value().has_value());
  CHECK(found.value()->language == "go");

  const auto absent = berth::user_recipe_get(p, "berthpanel-other");
  CHECK(absent.is_ok());
  CHECK(!absent.value().has_value());
}

TEST(resolve_recipe_prefers_the_user_recipe_over_the_builtin) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  // A user recipe shadowing the builtin memos entry.
  Recipe override = go_recipe("berthpanel-memos");
  override.owner = "me";
  override.name = "memos";
  CHECK(berth::user_recipe_store(p, override).is_ok());

  const auto resolved = berth::resolve_recipe(p, "berthpanel-memos");
  CHECK(resolved.is_ok());
  CHECK(resolved.value().owner == "me");
}

TEST(resolve_recipe_falls_back_to_the_builtin_catalog) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto resolved = berth::resolve_recipe(p, "berthpanel-memos");
  CHECK(resolved.is_ok());
  CHECK(resolved.value().service_name == "berthpanel-memos");

  const auto unknown = berth::resolve_recipe(p, "berthpanel-nosuch");
  CHECK(unknown.is_err());
}
