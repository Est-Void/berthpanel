#pragma once

#include <cstddef>
#include <filesystem>

#include "util/result.h"

namespace berth {

constexpr std::filesystem::perms kPermPrivate =
    std::filesystem::perms::owner_read |
    std::filesystem::perms::owner_write;  // 0600
constexpr std::filesystem::perms kPermPrivateDir =
    std::filesystem::perms::owner_all;  // 0700
constexpr std::filesystem::perms kPermShared =
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
    std::filesystem::perms::group_read |
    std::filesystem::perms::others_read;  // 0644, never for a secret

// create_directories() leaves umask bits behind — on a private dir that is a
// real leak.
[[nodiscard]] Result<void> ensure_dir(
    const std::filesystem::path& dir,
    std::filesystem::perms perms = kPermPrivateDir);

// Whole-file read; missing file is an Err, so callers wanting ENOENT-as-normal
// stat first.
[[nodiscard]] Result<std::string> read_file(const std::filesystem::path& path);

// Never follows a symlink, tolerates entries that vanish mid-walk, and refuses
// relative paths and "/".
[[nodiscard]] Result<std::size_t> remove_tree(
    const std::filesystem::path& root);

}
