// Tests for berth/wizard.h (stage 4.21, the engine half of the paste-a-URL
// flow). prepare_draft() needs the network and a scratch clone, so what is
// tested here is everything around it that decides something: the clash report
// (asked BEFORE the store, so the answer can still be "no") and the pure
// report both front ends render.

#include "berth/wizard.h"

#include <filesystem>
#include <string>

#include "berth/catalog.h"
#include "berth/recipes.h"
#include "berth/userrecipes.h"
#include "core/atomic.h"
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
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  return p;
}

Recipe go_recipe(const std::string& service_name, const std::string& commit) {
  Recipe r;
  r.owner = "someone";
  r.name = "myapp";
  r.commit = commit;
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

berth::DraftResult sample_result() {
  berth::DraftResult result;
  result.draft =
      go_recipe("berthpanel-myapp", "91ab2c4f7d3e5a6b8c0d1e2f3a4b5c6d7e8f9a0b");
  result.owner_repo = "someone/myapp";
  result.commit = result.draft.commit;
  berth::ToolStatus go;
  go.name = "go";
  go.present = true;
  go.version = "1.22";
  go.path = "/usr/bin/go";
  berth::ToolStatus bun;
  bun.name = "bun";
  bun.present = false;
  result.tools = {go, bun};
  result.warnings = {
      "start_cmd is an assumption — verify the app's real flags "
      "before deploying"};
  return result;
}

}  // namespace

TEST(wizard_a_free_name_reports_no_clash) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const Result<berth::DraftClash> clash =
      berth::find_recipe_clash(p, "berthpanel-myapp");
  CHECK(clash.is_ok());
  CHECK(!clash.value().exists);
  CHECK(clash.value().existing_commit.empty());
  CHECK(clash.value().existing_path.empty());
}

TEST(wizard_a_stored_recipe_is_a_clash_and_carries_its_pin) {
  // The question must be asked WITH the old pin in it: "overwrite 2b2192d with
  // 91ab2c4?" is a decision the user can make; "overwrite?" is not.
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(
      berth::store_draft(p,
                         go_recipe("berthpanel-myapp",
                                   "2b2192d4e153bd04f1d325b60fd880cf00d68b01"),
                         false)
          .is_ok());

  const Result<berth::DraftClash> clash =
      berth::find_recipe_clash(p, "berthpanel-myapp");
  CHECK(clash.is_ok());
  CHECK(clash.value().exists);
  CHECK(!clash.value().in_catalog);
  CHECK(clash.value().service_name == "berthpanel-myapp");
  CHECK(clash.value().existing_commit ==
        "2b2192d4e153bd04f1d325b60fd880cf00d68b01");
  CHECK(clash.value().existing_path == p.recipes_dir / "berthpanel-myapp.json");
}

TEST(wizard_a_builtin_name_is_a_clash_too) {
  // Shadowing a curated recipe is the same explicit act (§8) — the catalog
  // entry has no file, so in_catalog is what tells the caller that apart.
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const Result<berth::DraftClash> clash =
      berth::find_recipe_clash(p, "berthpanel-memos");
  CHECK(clash.is_ok());
  CHECK(clash.value().exists);
  CHECK(clash.value().in_catalog);
  CHECK(!clash.value().existing_commit.empty());
}

TEST(wizard_store_refuses_without_force_and_overwrites_with_it) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const Recipe old_recipe =
      go_recipe("berthpanel-myapp", "2b2192d4e153bd04f1d325b60fd880cf00d68b01");
  const Recipe new_recipe =
      go_recipe("berthpanel-myapp", "91ab2c4f7d3e5a6b8c0d1e2f3a4b5c6d7e8f9a0b");
  CHECK(berth::store_draft(p, old_recipe, false).is_ok());

  const Result<std::filesystem::path> refused =
      berth::store_draft(p, new_recipe, false);
  CHECK(refused.is_err());
  CHECK(refused.error().ctx().find("--force") != std::string::npos);

  const Result<std::filesystem::path> forced =
      berth::store_draft(p, new_recipe, true);
  CHECK(forced.is_ok());
  // The stored pin is the NEW one: an overwrite that silently kept the old
  // commit would deploy something nobody asked for.
  const Result<std::optional<Recipe>> stored =
      berth::user_recipe_get(p, "berthpanel-myapp");
  CHECK(stored.is_ok());
  CHECK(stored.value().has_value());
  CHECK(stored.value()->commit == new_recipe.commit);
}

TEST(wizard_store_refuses_a_recipe_the_validator_rejects) {
  // The store is the ONE validator's front door: a draft that cannot pass
  // validate_recipe must never reach the directory (§8, plan 4.19).
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  Recipe broken = go_recipe("berthpanel-broken", "");
  broken.res.memory_max.clear();  // memory_max is mandatory
  const Result<std::filesystem::path> stored =
      berth::store_draft(p, broken, false);
  CHECK(stored.is_err());
  CHECK(!std::filesystem::exists(p.recipes_dir / "berthpanel-broken.json"));
}

TEST(wizard_report_names_the_repo_the_pin_the_tools_and_the_warnings) {
  const berth::DraftResult result = sample_result();
  const std::string report = berth::render_draft_report(result, {});
  CHECK(report.find("draft berthpanel-myapp (go, 384M/200)") !=
        std::string::npos);
  CHECK(report.find("someone/myapp @ " + result.commit) != std::string::npos);
  CHECK(report.find(result.draft.start_cmd) != std::string::npos);
  CHECK(report.find("ok   go") != std::string::npos);
  CHECK(report.find("bun") != std::string::npos);
  CHECK(report.find("warning: start_cmd is an assumption") !=
        std::string::npos);
  // No clash, so no question in the report.
  CHECK(report.find("already exists") == std::string::npos);
}

TEST(wizard_report_puts_the_clash_question_before_anything_is_stored) {
  berth::DraftResult result = sample_result();
  berth::DraftClash clash;
  clash.exists = true;
  clash.service_name = "berthpanel-myapp";
  clash.existing_commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  const std::string report = berth::render_draft_report(result, clash);
  CHECK(report.find("already exists (user recipe)") != std::string::npos);
  CHECK(report.find("pinned 2b2192d") != std::string::npos);
  CHECK(report.find("storing needs an explicit overwrite") !=
        std::string::npos);

  berth::DraftClash curated;
  curated.exists = true;
  curated.in_catalog = true;
  curated.service_name = "berthpanel-memos";
  CHECK(berth::render_draft_report(result, curated).find("builtin catalog") !=
        std::string::npos);
}