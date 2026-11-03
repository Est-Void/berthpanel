#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "toolcheck.h"

namespace berth {

struct ToolchainPin {
  std::string name;
  std::string version;
  std::string url;
  std::string sha256;
  std::string inner_path;
};

[[nodiscard]] std::optional<ToolchainPin> pinned_toolchain(
    std::string_view name);

[[nodiscard]] std::filesystem::path toolbox_bin_dir(const Paths& paths);

[[nodiscard]] std::string path_with_toolbox(const Paths& paths,
                                            std::string_view base_path);

[[nodiscard]] bool pin_is_current(
    const std::filesystem::path& version_marker, std::string_view version);

[[nodiscard]] Result<std::filesystem::path> ensure_toolchain(
    std::string_view name, const Paths& paths, std::atomic<bool>* cancel);

[[nodiscard]] Result<std::vector<ToolStatus>> resolve_tools(
    const std::vector<std::string>& names, const Paths& paths,
    std::atomic<bool>* cancel);

}
