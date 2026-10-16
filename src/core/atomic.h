#pragma once

// Private files are never written in place: O_EXCL temp + fsync + rename, so a
// reader sees the old bytes or the new ones, never a prefix (dev.md §6.4).

#include <filesystem>
#include <string>
#include <string_view>

#include "util/fs.h"  // kPermPrivate
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<void> atomic_write(
    const std::filesystem::path& dest, std::string_view data,
    std::filesystem::perms perms = kPermPrivate);

// Copies `original` to a free `<name>.corrupt[.N]`; the corrupt-JSON path must
// never silently substitute an empty document.
[[nodiscard]] Result<std::filesystem::path> backup_unique(
    const std::filesystem::path& original);

}
