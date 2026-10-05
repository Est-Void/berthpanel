#include "preflight.h"

#include <string>

#include "../state.h"
#include "validate.h"

namespace berth {

namespace {

bool language_supported(const std::string& language) {
  return language == "go" || language == "node" || language == "python" ||
         language == "prebuilt";
}

}  // namespace

Result<PreflightReport> preflight_recipe(
    const RecipeLike& recipe, const std::vector<ToolStatus>& tools) {
  PreflightReport report;

  if (!validate_unit_name(recipe.service_name)) {
    report.items.push_back(
        {Blocker::kInvalidName, recipe.service_name});
  }
  const Result<void> repo = validate_owner_repo(recipe.owner_repo);
  if (repo.is_err()) {
    report.items.push_back({Blocker::kBadRepo, repo.error().ctx()});
  }
  const Result<void> sha = validate_commit_sha(recipe.commit);
  if (sha.is_err()) {
    report.items.push_back({Blocker::kUnpinnedCommit, recipe.commit});
  }
  if (!language_supported(recipe.language)) {
    report.items.push_back({Blocker::kUnsupportedLanguage, recipe.language});
  }

  for (const std::string& tool : recipe.tools) {
    bool present = false;
    for (const ToolStatus& status : tools) {
      if (status.name == tool && status.present) present = true;
    }
    if (!present) {
      report.items.push_back({Blocker::kMissingTool, tool});
    }
  }

  for (const std::vector<std::string>& step : recipe.build_steps) {
    const std::vector<std::string> matched = matched_dangerous_patterns(step);
    if (!matched.empty()) {
      report.items.push_back({Blocker::kForbiddenBuildStep, matched.front()});
    }
  }

  if (!recipe.has_memory_limit || !recipe.has_tasks_limit) {
    report.items.push_back({Blocker::kMissingLimits, "memory_max/tasks_max"});
  }

  if (recipe.start_cmd.empty()) {
    report.items.push_back({Blocker::kEmptyStartCmd, recipe.service_name});
  } else if (!matched_dangerous_patterns(recipe.start_cmd).empty()) {
    report.items.push_back(
        {Blocker::kForbiddenStartCmd,
         matched_dangerous_patterns(recipe.start_cmd).front()});
  }

  return report;
}

std::string render_preflight(const PreflightReport& report) {
  if (report.ready()) return "preflight: ready\n";
  std::string out = "preflight: blocked by " +
                    std::to_string(report.items.size()) + " problem(s)\n";
  for (const PreflightItem& item : report.items) {
    out += "  - ";
    out += std::to_string(static_cast<int>(item.blocker));
    out += ": ";
    out += item.detail;
    out += "\n";
  }
  return out;
}

}  // namespace berth