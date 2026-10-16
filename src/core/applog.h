#pragma once

// A failed log write is an Err the caller must see: transparency is the point.

#include <filesystem>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

// Falls back to "unknown-time" rather than emitting a bogus stamp.
[[nodiscard]] std::string log_timestamp();

// A newline in a field would split one entry into two; it becomes a space.
[[nodiscard]] std::string one_line(std::string_view field);

// Callers pass `line` already terminated with a newline.
[[nodiscard]] Result<void> append_log_line(const std::filesystem::path& file,
                                           const std::filesystem::path& dir,
                                           std::string_view line);

}
