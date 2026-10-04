#pragma once

// src/berth/toolcheck.h — the tool doctor: presence and version of the
// build tools a recipe declares (bun / pnpm / go / python3, dev.md §8).
// Presence comes from PATH resolution; the version line is read from the
// tool itself, so the report reflects what a build would actually run.
// Offline and cheap — no downloads.

#include <atomic>
#include <string>
#include <vector>

#include "../util/result.h"

namespace berth {

struct ToolStatus {
  std::string name;
  bool present = false;
  std::string path;
  std::string version;  // first version-looking line, empty when absent
};

[[nodiscard]] Result<std::vector<ToolStatus>> check_tools(
    const std::vector<std::string>& names, std::atomic<bool>* cancel);

// The catalog's declared tool set (stage 4.15 consumes this).
[[nodiscard]] std::vector<std::string> catalog_tools();

[[nodiscard]] std::string render_tool_report(const std::vector<ToolStatus>& tools);

}  // namespace berth
