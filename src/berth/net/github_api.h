#pragma once

// src/berth/net/github_api.h — GitHub content without git: the trees API
// for listings and codeload tarballs for checkouts (dev.md §5). Every
// endpoint takes a PINNED 40-hex commit, never a branch (dev.md §6.6).
//
// The response PARSERS here are pure and tested offline; the two fetch
// functions are the only networked part, and both go through the allowlisted
// client. The archive handling itself is tar.h's, which knows nothing about
// GitHub.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/paths.h"
#include "util/result.h"

namespace berth {

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
    const std::string& owner_repo, const std::string& sha, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel);

// Downloads owner/repo@sha (codeload tarball) and extracts under
// dest_dir; returns the single top-level directory of the archive.
[[nodiscard]] Result<std::filesystem::path> fetch_and_extract(
    const std::string& owner_repo, const std::string& sha,
    const std::filesystem::path& dest_dir, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel);

}  // namespace berth
