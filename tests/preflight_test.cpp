#include "berth/preflight.h"

#include <string>

#include "berth/toolcheck.h"
#include "runner.h"

using berth::Blocker;
using berth::preflight_recipe;
using berth::RecipeLike;
using berth::render_preflight;

namespace {

RecipeLike good_recipe() {
  RecipeLike r;
  r.service_name = "berthpanel-memos";
  r.owner_repo = "usememos/memos";
  r.commit = "0123456789abcdef0123456789abcdef01234567";
  r.language = "go";
  r.start_cmd = "{bin} --port {port}";
  r.tools = {"go"};
  r.build_steps = {{"go", "build", "-o", "memos", "."}};
  r.port = 8234;
  r.has_memory_limit = true;
  r.has_tasks_limit = true;
  return r;
}

std::vector<berth::ToolStatus> go_present() {
  return {{.name = "go",
           .present = true,
           .path = "/usr/bin/go",
           .version = "go1.24"}};
}

bool has_blocker(const berth::PreflightReport& report, Blocker blocker) {
  for (const auto& item : report.items) {
    if (item.blocker == blocker)
      return true;
  }
  return false;
}

}  // namespace

TEST(preflight_ready_recipe_passes) {
  const auto report = preflight_recipe(good_recipe(), go_present());
  CHECK(report.is_ok());
  CHECK(report.value().ready());
  CHECK(render_preflight(report.value()).find("ready") != std::string::npos);
}

TEST(preflight_rejects_unpinned_commit) {
  RecipeLike r = good_recipe();
  r.commit = "main";
  const auto report = preflight_recipe(r, go_present());
  CHECK(report.is_ok());
  CHECK(!report.value().ready());
  CHECK(has_blocker(report.value(), Blocker::kUnpinnedCommit));
}

TEST(preflight_rejects_missing_declared_tool) {
  RecipeLike r = good_recipe();
  r.tools = {"go", "bun"};  // bun not installed
  const auto report = preflight_recipe(r, go_present());
  CHECK(has_blocker(report.value(), Blocker::kMissingTool));
}

TEST(preflight_rejects_dangerous_build_step) {
  RecipeLike r = good_recipe();
  r.build_steps = {{"sh", "-c", "sudo rm -rf /"}};
  const auto report = preflight_recipe(r, go_present());
  CHECK(has_blocker(report.value(), Blocker::kForbiddenBuildStep));
}

TEST(preflight_requires_limits_name_and_start_cmd) {
  RecipeLike r = good_recipe();
  r.has_tasks_limit = false;
  r.service_name = "memos";
  r.start_cmd = "";
  const auto report = preflight_recipe(r, go_present());
  CHECK(has_blocker(report.value(), Blocker::kMissingLimits));
  CHECK(has_blocker(report.value(), Blocker::kInvalidName));
  CHECK(has_blocker(report.value(), Blocker::kEmptyStartCmd));
}

TEST(preflight_rejects_unknown_language) {
  RecipeLike r = good_recipe();
  r.language = "rust";
  const auto report = preflight_recipe(r, go_present());
  CHECK(has_blocker(report.value(), Blocker::kUnsupportedLanguage));
}