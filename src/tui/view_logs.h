#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/netstatus.h"
#include "../util/result.h"

namespace tui {

// One table decides which file a tab means, so a path is never built twice.
enum class LogFile {
  kDeploy,
  kNet,
  kCrash,
};

inline constexpr std::size_t kLogFileCount = 3;

[[nodiscard]] std::string_view log_file_label(LogFile file);
[[nodiscard]] std::string_view log_file_path_hint(LogFile file);
[[nodiscard]] LogFile log_file_at(std::size_t index);
[[nodiscard]] std::size_t log_file_index(LogFile file);

// An empty crash.log is GOOD NEWS; that copy is deliberate.
[[nodiscard]] std::vector<std::string> log_empty_lines(LogFile file);

// A failed read, NOT an absent file: an absent file is success, see tail.h.
[[nodiscard]] std::vector<std::string> log_error_lines(
    LogFile file, const std::string& error);

// Empty when nothing has been requested: no row of zeroes on the header.
[[nodiscard]] std::string net_banner(const berth::NetSnapshot& snapshot);

[[nodiscard]] std::string net_banner_note(const berth::NetSnapshot& snapshot);

// An unread log shows no count at all: "(0)" is a claim, not a measurement.
[[nodiscard]] std::string log_tabs_line(
    LogFile selected, const std::vector<std::optional<std::size_t>>& counts);

struct LogWindow {
  std::size_t first;
  std::size_t count;
};

// scroll counts UP from the newest, so 0 pins the bottom; a huge scroll clamps
// at the oldest still-kept line and never shrinks the window.
[[nodiscard]] LogWindow log_window_range(std::size_t total, std::size_t scroll,
                                         std::size_t visible);

enum class StatusTone {
  kNeutral,
  kOk,
  kBad,
};

[[nodiscard]] StatusTone classify_status(std::string_view text);

}
