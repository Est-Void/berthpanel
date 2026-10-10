#pragma once

// src/berth/preflight.h — the prerequisite gate a deploy may not skip.
// Pure logic over a structural view of a recipe, so it is testable
// without spawning anything and cannot drift from recipes.cpp: the
// RecipeLike struct IS the contract. A report with any item is not ready.

#include <cstdint>
#include <string>
#include <vector>

#include "../util/result.h"
#include "toolcheck.h"

namespace berth {

enum class Blocker {
  kInvalidName,
  kBadRepo,
  kUnpinnedCommit,
  kUnsupportedLanguage,
  kMissingTool,
  kForbiddenBuildStep,
  kMissingLimits,
  kEmptyStartCmd,
  kForbiddenStartCmd,
};

struct RecipeLike {
  std::string service_name;
  std::string owner_repo;
  std::string commit;
  std::string language;  // go | node | python | prebuilt
  std::string start_cmd;
  std::vector<std::string> tools;
  std::vector<std::vector<std::string>> build_steps;
  std::uint16_t port = 0;
  bool has_memory_limit = false;
  bool has_tasks_limit = false;
};

struct PreflightItem {
  Blocker blocker = Blocker::kInvalidName;
  std::string detail;
};

struct PreflightReport {
  std::vector<PreflightItem> items;
  [[nodiscard]] bool ready() const { return items.empty(); }
};

[[nodiscard]] Result<PreflightReport> preflight_recipe(
    const RecipeLike& recipe, const std::vector<ToolStatus>& tools);

[[nodiscard]] std::string render_preflight(const PreflightReport& report);

}  // namespace berth