#pragma once

// src/berth/goenv.h — Go toolchain install and module-cache seeding with
// resumable segments. Seeding uses the module cache's own layout, which is
// also a valid GOPROXY=file:// tree, so no zip surgery is involved: go
// unpacks on its own. That is why miniz is NOT vendored (dev.md §2 lists
// it for "the Go module cache") — vendoring a zip library nothing calls
// would be dead code by §11. Vendoring it is a one-file change the moment
// something really needs to read a zip.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>

#include "../paths.h"
#include "../util/result.h"
#include "toolbox.h"

namespace berth {

// Go's module-path escaping: an uppercase letter is stored as '!' plus its
// lowercase form (github.com/Masterminds/semver -> github.com/!masterminds/...).
[[nodiscard]] Result<std::string> escape_module_path(std::string_view module);

[[nodiscard]] Result<std::string> range_header_value(std::size_t have,
                                                      std::size_t total);

[[nodiscard]] Result<std::size_t> current_segment_size(
    const std::filesystem::path& partial);

[[nodiscard]] Result<std::size_t> append_segment(
    const std::filesystem::path& partial, const std::string_view chunk);

// Size + SHA-256 checked, then renamed into place. A resumed download that
// does not add up fails here and never becomes the installed file.
[[nodiscard]] Result<void> finalize_segmented(
    const std::filesystem::path& partial, const std::filesystem::path& dest,
    const std::string& expected_sha256, std::size_t expected_size);

struct ModuleFiles {
  std::string info;  // {"Version":"v1.2.3"}
  std::string mod;   // go.mod
  std::string zip;   // module zip, already prefixed module@version/ by go
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

// The environment a sandboxed go build needs: our toolchain, our cache,
// our file:// proxy, and GOTOOLCHAIN=local so go never tries to fetch a
// different toolchain from the network mid-build.
[[nodiscard]] std::vector<std::string> go_build_env(
    const std::filesystem::path& go_root,
    const std::filesystem::path& module_cache, std::size_t max_procs = 4);

}  // namespace berth