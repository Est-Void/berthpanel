#pragma once

// src/atomic.h — durable file writes. A reader of a private file must see
// either the old bytes or the new ones, never a prefix: every write lands in
// an O_EXCL temp file, is fsync'd, and is then renamed over the target
// (dev.md §6.4). Nothing here ever truncates the destination in place.

#include <filesystem>
#include <string>
#include <string_view>

#include "util/fs.h"  // kPermPrivate
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<void> atomic_write(
    const std::filesystem::path& dest, std::string_view data,
    std::filesystem::perms perms = kPermPrivate);

// Copies `original` aside to a free `<name>.corrupt[.N]` before anything
// overwrites or replaces it. Used by the corrupt-JSON path, which must never
// silently substitute an empty document.
[[nodiscard]] Result<std::filesystem::path> backup_unique(
    const std::filesystem::path& original);

}  // namespace berth
