#pragma once

// src/berth/rawfetch.h — GitHub content without git: the trees API for
// listings and codeload tarballs for checkouts (dev.md §5). The tar
// reader is minimal and read-only, and REJECTS loudly: absolute paths,
// ".." components, device nodes, symlinks that escape the destination,
// and any malformed or truncated structure (§2, §11 — a skipped unsafe
// entry is a bug, not a policy). Commits are pinned SHAs only (§6.6).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../paths.h"
#include "../util/result.h"

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
[[nodiscard]] Result<std::size_t> tar_extract(std::string_view bytes,
                                              const std::filesystem::path& dest);

// Same, for a gzipped tar — what codeload and GitHub release assets
// actually serve. The gzip layer is explicit rather than sniffed.
[[nodiscard]] Result<std::size_t> tar_extract_gz(
    std::string_view gzipped, const std::filesystem::path& dest,
    std::size_t max_out = 1024ull * 1024 * 1024);

struct TreeFile {
  std::string path;
  std::string sha;  // blob sha
  std::uint64_t size = 0;
};

// Parses a trees API response. `truncated: true` is a loud error: an
// incomplete listing must never look complete (§11).
[[nodiscard]] Result<std::vector<TreeFile>> parse_tree_response(
    std::string_view body);

[[nodiscard]] Result<std::vector<TreeFile>> fetch_tree(
    const std::string& owner_repo, const std::string& sha,
    const Paths& paths, const std::string* token, std::atomic<bool>* cancel);

// Downloads owner/repo@sha (codeload tarball) and extracts under
// dest_dir; returns the single top-level directory of the archive.
[[nodiscard]] Result<std::filesystem::path> fetch_and_extract(
    const std::string& owner_repo, const std::string& sha,
    const std::filesystem::path& dest_dir, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel);

}  // namespace berth
