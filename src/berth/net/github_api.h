#pragma once

// Every endpoint takes a pinned 40-hex commit, never a branch (§6.6).

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
  std::string sha;
  std::uint64_t size = 0;
};

// An incomplete listing must never look complete (§11).
[[nodiscard]] Result<std::vector<TreeFile>> parse_tree_response(
    std::string_view body);

// Pure: the default branch, from the repos API response.
[[nodiscard]] Result<std::string> parse_default_branch(std::string_view body);

// Pure: the 40-hex SHA of a commits/<ref> response.
[[nodiscard]] Result<std::string> parse_commit_sha(std::string_view body);

// Networked, and no token is attached: public repos only (§6.3).
[[nodiscard]] Result<std::string> resolve_head_sha(const Paths& paths,
                                                   const std::string& owner_repo,
                                                   const std::string& ref,
                                                   std::atomic<bool>* cancel);

[[nodiscard]] Result<std::vector<TreeFile>> fetch_tree(
    const std::string& owner_repo, const std::string& sha, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel);

[[nodiscard]] Result<std::filesystem::path> fetch_and_extract(
    const std::string& owner_repo, const std::string& sha,
    const std::filesystem::path& dest_dir, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel);

}
