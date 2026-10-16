#pragma once

// Presence from PATH, version from the tool itself. Offline, no downloads.

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

[[nodiscard]] std::vector<std::string> catalog_tools();

[[nodiscard]] std::string render_tool_report(
    const std::vector<ToolStatus>& tools);

}
