#pragma once

// Seeding writes the module cache's own layout, a valid GOPROXY=file:// tree:
// go unpacks on its own, so no zip library is vendored.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>

#include "../core/paths.h"
#include "../util/result.h"
#include "toolbox.h"

namespace berth {

[[nodiscard]] Result<std::string> escape_module_path(std::string_view module);

[[nodiscard]] Result<std::string> range_header_value(std::size_t have,
                                                     std::size_t total);

[[nodiscard]] Result<std::size_t> current_segment_size(
    const std::filesystem::path& partial);

[[nodiscard]] Result<std::size_t> append_segment(
    const std::filesystem::path& partial, const std::string_view chunk);

[[nodiscard]] Result<void> finalize_segmented(
    const std::filesystem::path& partial, const std::filesystem::path& dest,
    const std::string& expected_sha256, std::size_t expected_size);

struct ModuleFiles {
  std::string info;
  std::string mod;
  std::string zip;  // module zip, already prefixed module@version/ by go
};

[[nodiscard]] Result<std::filesystem::path> write_module_cache_entry(
    const std::filesystem::path& module_cache, const std::string& module,
    const std::string& version, const ModuleFiles& files);

[[nodiscard]] Result<std::filesystem::path> install_go_toolchain(
    const PinnedTool& archive, const std::filesystem::path& tools_dir,
    const Paths& paths, std::atomic<bool>* cancel);

[[nodiscard]] Result<void> seed_module(
    const std::string& module, const std::string& version,
    const std::filesystem::path& module_cache,
    const std::vector<std::pair<std::string, std::string>>& url_to_sha,
    const Paths& paths, std::atomic<bool>* cancel);

// GOTOOLCHAIN=local: go must never fetch another toolchain mid-build.
[[nodiscard]] std::vector<std::string> go_build_env(
    const std::filesystem::path& go_root,
    const std::filesystem::path& module_cache, std::size_t max_procs = 4);

}
