#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "util/result.h"

namespace berth {

struct Tail {
  std::vector<std::string> lines;  // oldest first, at most `wanted` of them
  bool truncated = false;          // the file is longer than the byte window
  bool existed = false;  // false = the file does not exist (not an error)
};

// ENOENT is success with existed = false; an empty crash.log is the normal
// state and an error box over it is noise. Every other failure is a real Err.
inline constexpr std::size_t kTailWindowBytes = std::size_t{64} * 1024;

// A window starting mid-line drops the fragment instead of reporting it as a
// truncated entry.
[[nodiscard]] std::vector<std::string> split_log_lines(std::string_view text);

[[nodiscard]] Result<Tail> tail_lines(const std::filesystem::path& path,
                                      std::size_t wanted);

[[nodiscard]] Tail tail_from_text(std::string_view text, std::size_t wanted,
                                  bool window_started_mid_line = false);

}
