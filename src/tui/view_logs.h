#pragma once

// src/tui/view_logs.h — the Logs view model (plan stage 6.6) as pure functions
// over data. No FTXUI, no threads: the file reads are tail_lines() calls on a
// worker, and everything drawn — the NET banner, the tab bar, the section
// notes — comes from here.
//
// The NET banner is the point of this stage: §6.1 promises that every outbound
// request is logged, and a counter the user can see without leaving the panel
// is what turns "we log everything" from a claim in a README into something
// they can watch. It reads netstatus (one atomic word), so it can be rendered
// on EVERY frame without blocking.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/netstatus.h"
#include "../util/result.h"

namespace tui {

// The three logs. Which file a tab means is decided here, in one table, so a
// path can never be built in two places and drift.
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

// What the file has to say when it has nothing to say. An empty crash.log is
// GOOD NEWS, and it is the one log a user opens hoping it stays that way.
[[nodiscard]] std::vector<std::string> log_empty_lines(LogFile file);

// Shown when the tail could not be read. An absent file is NOT this — see
// tail.h; this is for the real failures.
[[nodiscard]] std::vector<std::string> log_error_lines(
    LogFile file, const std::string& error);

// The header's NET banner: what THIS session asked for and how much of it was
// refused. Empty when nothing has been requested yet, so the header does not
// carry a row of zeroes on a panel that has not touched the network.
[[nodiscard]] std::string net_banner(const berth::NetSnapshot& snapshot);

// What the banner means in words, drawn under it once per refresh. Names the
// allowlist rule so the number is not just a number (§6.1).
[[nodiscard]] std::string net_banner_note(const berth::NetSnapshot& snapshot);

// The tab line: which log is selected, and how many lines of it are on
// screen. A log that has NOT been read yet shows no count at all — "(0)" for a
// file nobody has looked at is a claim, not a measurement.
[[nodiscard]] std::string log_tabs_line(
    LogFile selected, const std::vector<std::optional<std::size_t>>& counts);

}  // namespace tui