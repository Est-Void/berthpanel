#pragma once

// src/core/json.h — JSON file I/O with the corruption policy attached.
// A document that will not parse is COPIED ASIDE and the error says so; it
// is never read as empty, because a silently empty registry would make the
// panel forget every service it manages (dev.md §11). Writes are atomic 0600.

#include <filesystem>

#include <nlohmann/json.hpp>

#include "util/result.h"

namespace berth {

// Parses `path`. On a parse error the file is quarantined (see
// quarantine_corrupt) and the returned error names both the parse failure and
// where the bad bytes went.
[[nodiscard]] Result<nlohmann::json> json_load(
    const std::filesystem::path& path);

// Atomic 0600 write of `value` pretty-printed with a trailing newline.
[[nodiscard]] Result<void> json_store(const std::filesystem::path& path,
                                      const nlohmann::json& value);

// Copies `path` aside to a free `<name>.corrupt[.N]` and returns the new
// path, so the original is never destroyed by a recovery path.
[[nodiscard]] Result<std::filesystem::path> quarantine_corrupt(
    const std::filesystem::path& path);

}  // namespace berth
