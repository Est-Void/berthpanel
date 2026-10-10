#pragma once

// src/core/applog.h — the ONE append-only log sink. net.log and deploy.log
// are the same thing with a different filename and a different line format,
// so the O_APPEND 0600 discipline, the EINTR-safe write, the newline
// sanitising and the ISO-8601 stamp live here exactly once. A failed log
// write is an error the caller must see: transparency is the point.

#include <filesystem>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

// "2026-10-07T12:34:56.789Z". Falls back to "unknown-time" rather than
// lying; no caller depends on the width.
[[nodiscard]] std::string log_timestamp();

// Newlines inside a field would split one entry into two records, so they
// become spaces. Every field of every sink goes through this.
[[nodiscard]] std::string one_line(std::string_view field);

// Creates `dir` on the first ENOENT, then appends `line` (already ending in
// "\n") to `file` with a single O_APPEND write.
[[nodiscard]] Result<void> append_log_line(const std::filesystem::path& file,
                                           const std::filesystem::path& dir,
                                           std::string_view line);

}  // namespace berth
