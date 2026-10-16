#pragma once

// Rejects loudly rather than skipping: absolute paths, "..", devices,
// escaping links, base-256 sizes, GNU long names, anything malformed.
// Scope is ustar plus the pax extensions `git archive` emits.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace berth {

// Safe = relative, no "..", no NUL; "." passes.
[[nodiscard]] bool archive_path_safe(std::string_view path);

// Absolute link targets always escape.
[[nodiscard]] bool link_target_escapes(std::string_view from_dir,
                                       std::string_view target);

struct TarEntry {
  std::string name;
  std::uint64_t size = 0;
  char typeflag = '0';  // '0','5','1','2' file/dir/hardlink/symlink
  std::string link_target;
  std::uint32_t mode = 0;
  std::size_t data_offset = 0;  // into the archive bytes (files only)
};

[[nodiscard]] Result<std::vector<TarEntry>> tar_list(std::string_view bytes);

// One unsafe or duplicate entry fails the WHOLE extraction loudly; partial
// files may remain and are the caller's work-dir to clean up.
[[nodiscard]] Result<std::size_t> tar_extract(
    std::string_view bytes, const std::filesystem::path& dest);

// The gzip layer is explicit rather than sniffed.
[[nodiscard]] Result<std::size_t> tar_extract_gz(
    std::string_view gzipped, const std::filesystem::path& dest,
    std::size_t max_out = 1024ull * 1024 * 1024);

}
