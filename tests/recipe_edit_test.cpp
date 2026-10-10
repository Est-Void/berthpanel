// Tests for recipe_edit.h (dev.md §8): the pure apply (valid edit, invalid
// start_cmd and limits rejected), the update round-trip (persists, atomic,
// builtin-only refused) and the CLI flag parser. No env, no network.

#include "berth/recipe_edit.h"

#include <filesystem>
#include <string>
#include <vector>

#include "berth/recipes.h"
#include "berth/userrecipes.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::Recipe;
using berth::RecipeEdits;
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

TEST(recipe_edit_apply_changes_only_the_set_fields) {
  const Recipe base = go_recipe();
  RecipeEdits edits;
  edits.start_cmd = "{self} --addr 127.0.0.1 --port {port} --data {project}/d";
  edits.port = 9999;
  edits.memory_max = "768M";
  edits.tasks_max = 400;

  const auto edited = berth::apply_recipe_edits(base, edits);
  CHECK(edited.is_ok());
  CHECK(edited.value().start_cmd == edits.start_cmd.value());
  CHECK(edited.value().port == 9999);
  CHECK(edited.value().res.memory_max == "768M");
  CHECK(edited.value().res.tasks_max == 400);
  // Identity/pin fields are untouched by the engine.
  CHECK(edited.value().owner == base.owner);
  CHECK(edited.value().commit == base.commit);
  CHECK(edited.value().build_steps == base.build_steps);
}

TEST(recipe_edit_apply_rejects_a_start_cmd_without_loopback) {
  RecipeEdits edits;
  edits.start_cmd = "{self} --port {port}";  // no explicit loopback
  const auto edited = berth::apply_recipe_edits(go_recipe(), edits);
  CHECK(edited.is_err());
  CHECK(edited.error().ctx().find("loopback") != std::string::npos);

  RecipeEdits wildcard;
  wildcard.start_cmd = "{self} --addr 0.0.0.0 --port {port}";
  const auto rejected = berth::apply_recipe_edits(go_recipe(), wildcard);
  CHECK(rejected.is_err());
}

TEST(recipe_edit_apply_sets_a_declared_bind) {
  // The BYO no-bind-flag case: start_cmd is just {self} and the author
  // declares loopback; a fixed port drops the {port} requirement.
  RecipeEdits edits;
  edits.start_cmd = "{self}";
  edits.bind = "127.0.0.1";
  edits.port = 46877;
  const auto edited = berth::apply_recipe_edits(go_recipe(), edits);
  CHECK(edited.is_ok());
  CHECK(edited.value().start_cmd == "{self}");
  CHECK(edited.value().bind == "127.0.0.1");
  CHECK(edited.value().port == 46877);
}

TEST(recipe_edit_apply_rejects_a_wildcard_bind) {
  RecipeEdits edits;
  edits.start_cmd = "{self}";
  edits.bind = "0.0.0.0";
  edits.port = 46877;
  const auto edited = berth::apply_recipe_edits(go_recipe(), edits);
  CHECK(edited.is_err());
  CHECK(edited.error().ctx().find("wildcard") != std::string::npos);
}

TEST(recipe_edit_apply_rejects_empty_limits) {
  RecipeEdits no_memory;
  no_memory.memory_max = "";
  CHECK(berth::apply_recipe_edits(go_recipe(), no_memory).is_err());

  RecipeEdits no_tasks;
  no_tasks.tasks_max = 0;
  const auto rejected = berth::apply_recipe_edits(go_recipe(), no_tasks);
  CHECK(rejected.is_err());
  CHECK(rejected.error().ctx().find("memory_max") != std::string::npos);
}

TEST(recipe_edit_apply_without_fields_is_an_error) {
  const auto edited = berth::apply_recipe_edits(go_recipe(), RecipeEdits{});
  CHECK(edited.is_err());
  CHECK(edited.error().ctx().find("no fields") != std::string::npos);
}

TEST(recipe_edit_update_persists_the_edit_and_reports_before_after) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::user_recipe_store(p, go_recipe(), false).is_ok());

  RecipeEdits edits;
  edits.start_cmd = "{self} --addr 127.0.0.1 --port {port} --fixed";
  edits.port = 8123;
  const auto updated = berth::user_recipe_update(p, "berthpanel-myapp", edits);
  CHECK(updated.is_ok());
  CHECK(updated.value().before.port == 0);
  CHECK(updated.value().after.port == 8123);
  CHECK(updated.value().stored_path == p.recipes_dir / "berthpanel-myapp.json");

  // Persisted (and nothing else changed).
  const auto loaded = berth::user_recipes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 1);
  CHECK(loaded.value().front().start_cmd == edits.start_cmd.value());
  CHECK(loaded.value().front().port == 8123);
  CHECK(loaded.value().front().res.memory_max == "384M");
}

TEST(recipe_edit_update_refuses_a_builtin_only_name) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  RecipeEdits edits;
  edits.tasks_max = 300;
  const auto refused = berth::user_recipe_update(p, "berthpanel-memos", edits);
  CHECK(refused.is_err());
  CHECK(refused.error().ctx().find("builtin") != std::string::npos);
  // Nothing was written for the builtin name.
  CHECK(!std::filesystem::exists(p.recipes_dir / "berthpanel-memos.json"));
}

TEST(recipe_edit_update_refuses_an_unknown_name) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  RecipeEdits edits;
  edits.tasks_max = 300;
  const auto refused = berth::user_recipe_update(p, "berthpanel-nosuch", edits);
  CHECK(refused.is_err());
  CHECK(refused.error().ctx().find("no user recipe") != std::string::npos);
}

TEST(recipe_edit_parse_maps_every_flag) {
  RecipeEdits edits;
  const std::vector<std::string> tokens = {
      "--start-cmd",   "{self} --addr 127.0.0.1 --port {port}",
      "--port",        "8080",
      "--memory",      "512M",
      "--tasks",       "200",
      "--health",      "/healthz",
      "--name",        "Pretty Name",
      "--description", "a description with spaces"};
  const auto parsed = berth::parse_recipe_edit_args(tokens);
  CHECK(parsed.is_ok());
  CHECK(parsed.value().start_cmd.has_value());
  CHECK(parsed.value().start_cmd.value() == tokens[1]);
  CHECK(parsed.value().port.value() == 8080);
  CHECK(parsed.value().memory_max.value() == "512M");
  CHECK(parsed.value().tasks_max.value() == 200);
  CHECK(parsed.value().health_path.value() == "/healthz");
  CHECK(parsed.value().display_name.value() == "Pretty Name");
  CHECK(parsed.value().description.value() == "a description with spaces");
}

TEST(recipe_edit_parse_rejects_bad_input_loudly) {
  CHECK(berth::parse_recipe_edit_args({}).is_err());  // no flags
  CHECK(berth::parse_recipe_edit_args({"--bogus", "x"}).is_err());
  CHECK(berth::parse_recipe_edit_args({"--start-cmd"}).is_err());  // no value
  CHECK(berth::parse_recipe_edit_args({"--bind"}).is_err());       // no value
  CHECK(berth::parse_recipe_edit_args({"--port", "0"}).is_err());
  CHECK(berth::parse_recipe_edit_args({"--port", "70000"}).is_err());
  CHECK(berth::parse_recipe_edit_args({"--port", "80O"}).is_err());
  CHECK(berth::parse_recipe_edit_args({"--tasks", "0"}).is_err());
  CHECK(berth::parse_recipe_edit_args({"--tasks", "-1"}).is_err());
}

TEST(recipe_edit_parse_maps_bind) {
  const auto parsed = berth::parse_recipe_edit_args(
      {"--start-cmd", "{self}", "--bind", "127.0.0.1", "--port", "46877"});
  CHECK(parsed.is_ok());
  CHECK(parsed.value().start_cmd.value() == "{self}");
  CHECK(parsed.value().bind.has_value());
  CHECK(parsed.value().bind.value() == "127.0.0.1");
  CHECK(parsed.value().port.value() == 46877);
}

TEST(recipe_edit_diff_names_each_changed_field) {
  Recipe before = go_recipe();
  Recipe after = before;
  after.start_cmd = "{self} --addr 127.0.0.1 --port {port} --fixed";
  after.port = 8123;
  const std::string diff = berth::render_recipe_edit_diff(before, after);
  CHECK(diff.find("start_cmd:") != std::string::npos);
  CHECK(diff.find("port: 0 -> 8123") != std::string::npos);
  CHECK(diff.find("res.memory_max") == std::string::npos);  // unchanged

  CHECK(
      berth::render_recipe_edit_diff(before, before).find("no field changed") !=
      std::string::npos);
}
