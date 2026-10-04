#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

constexpr std::filesystem::perms kPermPrivate =
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;  // 0600
constexpr std::filesystem::perms kPermPrivateDir = std::filesystem::perms::owner_all;  // 0700

[[nodiscard]] Result<void> ensure_dir(const std::filesystem::path& dir,
                                      std::filesystem::perms perms = kPermPrivateDir);

[[nodiscard]] Result<std::string> read_file(const std::filesystem::path& path);

[[nodiscard]] Result<void> atomic_write(
    const std::filesystem::path& dest, std::string_view data,
    std::filesystem::perms perms = kPermPrivate);

[[nodiscard]] Result<std::filesystem::path> backup_unique(
    const std::filesystem::path& original);

}  // namespace berth
