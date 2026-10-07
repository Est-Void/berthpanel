#pragma once

// src/util/fs.h — directory and file primitives shared by every layer.
// Generic POSIX work only: no policy, no XDG layout, no berth names.

#include <cstddef>
#include <filesystem>

#include "util/result.h"

namespace berth {

constexpr std::filesystem::perms kPermPrivate =
    std::filesystem::perms::owner_read |
    std::filesystem::perms::owner_write;  // 0600
constexpr std::filesystem::perms kPermPrivateDir =
    std::filesystem::perms::owner_all;  // 0700
// World-readable but not writable: for files that carry no secrets.
constexpr std::filesystem::perms kPermShared =
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
    std::filesystem::perms::group_read |
    std::filesystem::perms::others_read;  // 0644

// mkdir -p, then chmod to the exact mode (create_directories leaves umask
// bits behind, which for a private dir would be a real leak).
[[nodiscard]] Result<void> ensure_dir(
    const std::filesystem::path& dir,
    std::filesystem::perms perms = kPermPrivateDir);

// Whole-file read. Missing files are an error; callers that treat ENOENT as
// a normal state stat first.
[[nodiscard]] Result<std::string> read_file(const std::filesystem::path& path);

// Recursive delete that never follows a symlink, tolerates entries that
// vanish mid-walk, and refuses relative paths and "/". Returns the number
// of entries removed.
[[nodiscard]] Result<std::size_t> remove_tree(
    const std::filesystem::path& root);

}  // namespace berth
