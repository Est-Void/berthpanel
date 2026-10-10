#pragma once

// src/berth/net/tar.h — the minimal read-only tar reader. NO NETWORK, NO
// JSON: this is what codeload and release assets serve, and it is kept
// separate so its safety rules can be read on their own and tested offline.
//
// It rejects loudly rather than skipping: absolute paths, "..", device
// nodes, symlinks that escape the destination, a base-256 size field, GNU
// long names, and any malformed or truncated structure. A silently skipped
// unsafe entry would be a policy hole, not a feature (dev.md §2, §11).
//
// Scope is ustar plus the pax extensions `git archive` actually emits
// (path, linkpath, size overrides and the global header).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace berth {

// A path inside an archive is safe when it is relative, has no ".."
// component, and contains no NUL. "." components cannot escape and pass.
[[nodiscard]] bool archive_path_safe(std::string_view path);

// Would a link whose target is placed in `from_dir` (relative to the
// extraction root) escape the root? Absolute targets always escape.
[[nodiscard]] bool link_target_escapes(std::string_view from_dir,
                                       std::string_view target);

struct TarEntry {
  std::string name;
  std::uint64_t size = 0;
  char typeflag = '0';  // '0' file, '5' dir, '1' hardlink, '2' symlink,
                        // 'x'/'g' pax records are applied or skipped
  std::string link_target;
  std::uint32_t mode = 0;
  std::size_t data_offset = 0;  // into the archive bytes (files only)
};

// Lists entries. Err on any malformed or truncated structure.
[[nodiscard]] Result<std::vector<TarEntry>> tar_list(std::string_view bytes);

// Extracts into `dest` (created if missing, perms 0700). One unsafe or
// duplicate entry fails the WHOLE extraction loudly; partial files may
// remain on disk and are the caller's work-dir to clean up.
[[nodiscard]] Result<std::size_t> tar_extract(
    std::string_view bytes, const std::filesystem::path& dest);

// Same, for a gzipped tar. The gzip layer is explicit rather than sniffed.
[[nodiscard]] Result<std::size_t> tar_extract_gz(
    std::string_view gzipped, const std::filesystem::path& dest,
    std::size_t max_out = 1024ull * 1024 * 1024);

}  // namespace berth
