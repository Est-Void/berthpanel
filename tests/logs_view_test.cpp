// Tests for the Logs view model (stage 6.6): the NET banner and the three log
// viewers. The NET banner is the §6.1 promise made visible, so its tests are
// about honesty: it counts what THIS session asked for, it separates refused
// from allowed, and it stays silent when there is nothing to say.

#include "tui/view_logs.h"

#include <optional>
#include <string>
#include <vector>

#include "runner.h"

namespace {

bool contains(const std::vector<std::string>& haystack,
              const std::string_view needle) {
  for (const std::string& line : haystack) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

berth::NetSnapshot snapshot(const std::uint64_t allowed,
                            const std::uint64_t denied,
                            const std::string& last_host) {
  berth::NetSnapshot state;
  state.allowed = allowed;
  state.denied = denied;
  state.total = allowed + denied;
  state.last_host = last_host;
  return state;
}

}  // namespace

TEST(net_banner_counts_this_session_and_separates_refusals) {
  const std::string banner =
      tui::net_banner(snapshot(7, 2, "raw.githubusercontent.com"));
  CHECK(banner.find("NET 9") != std::string::npos);
  CHECK(banner.find("(2 denied)") != std::string::npos);
  CHECK(banner.find("raw.githubusercontent.com") != std::string::npos);
}

TEST(net_banner_is_silent_before_the_first_request) {
  // A row of zeroes is a claim about nothing; the header stays clean.
  CHECK(tui::net_banner(snapshot(0, 0, "")).empty());
  CHECK(tui::net_banner_note(snapshot(0, 0, "")).find("allowlist") !=
        std::string::npos);
}

TEST(net_banner_note_says_the_count_happens_before_the_log_line) {
  // This is the property that makes the number trustworthy: it cannot lag
  // behind net.log, because it is bumped before the write.
  const std::string note =
      tui::net_banner_note(snapshot(3, 1, "api.github.com"));
  CHECK(note.find("3 allowed") != std::string::npos);
  CHECK(note.find("1 denied") != std::string::npos);
  CHECK(note.find("before the log line") != std::string::npos);
}

TEST(net_banner_note_calls_out_a_refusal) {
  const std::string note =
      tui::net_banner_note(snapshot(1, 4, "evil.example.com"));
  CHECK(note.find("DENIED line") != std::string::npos);
  // With no refusals there is nothing to point at.
  CHECK(tui::net_banner_note(snapshot(9, 0, "github.com")).find("DENIED") ==
        std::string::npos);
}

TEST(log_files_are_the_three_named_logs_in_a_fixed_order) {
  CHECK(tui::kLogFileCount == 3);
  CHECK(tui::log_file_label(tui::LogFile::kDeploy) == "deploy.log");
  CHECK(tui::log_file_label(tui::LogFile::kNet) == "net.log");
  CHECK(tui::log_file_label(tui::LogFile::kCrash) == "crash.log");
  CHECK(tui::log_file_at(0) == tui::LogFile::kDeploy);
  CHECK(tui::log_file_at(1) == tui::LogFile::kNet);
  CHECK(tui::log_file_at(2) == tui::LogFile::kCrash);
  // index() is how the line count lands in the right slot.
  CHECK(tui::log_file_index(tui::LogFile::kNet) == 1);
  CHECK(tui::log_file_path_hint(tui::LogFile::kNet).find("DENIED") !=
        std::string_view::npos);
}

TEST(log_tabs_line_marks_the_selection_and_shows_the_counts) {
  const std::string line = tui::log_tabs_line(
      tui::LogFile::kNet, {12, std::optional<std::size_t>(340), 0});
  CHECK(line.find("deploy.log (12)") != std::string::npos);
  CHECK(line.find("net.log (340)") != std::string::npos);
  CHECK(line.find("crash.log (0)") != std::string::npos);
  // The marker brackets exactly one entry.
  const std::size_t first = line.find("\xe2\x96\xb6");
  const std::size_t second = line.find("\xe2\x96\xb6", first + 1);
  CHECK(second == std::string::npos);
  CHECK(line.find("net.log", first) < line.find("net.log", first) + 10);
}

TEST(log_tabs_line_shows_no_count_for_a_log_nobody_read_yet) {
  // "(0)" for a file that has never been opened is a claim, not a
  // measurement — an unread log gets no number at all.
  const std::string line = tui::log_tabs_line(
      tui::LogFile::kDeploy,
      {std::nullopt, std::optional<std::size_t>(7), std::nullopt});
  CHECK(line.find("deploy.log (") == std::string::npos);
  CHECK(line.find("net.log (7)") != std::string::npos);
  CHECK(line.find("crash.log (") == std::string::npos);

  // And before any read at all.
  const std::string unread = tui::log_tabs_line(tui::LogFile::kDeploy, {});
  CHECK(unread.find('(') == std::string::npos);
}

TEST(an_empty_crash_log_is_good_news_and_says_so) {
  // The one log a user opens hoping it stays empty. An error box here would be
  // exactly backwards.
  const std::vector<std::string> lines =
      tui::log_empty_lines(tui::LogFile::kCrash);
  CHECK(contains(lines, "nothing has crashed"));
  CHECK(contains(lines, "absent file and an empty one mean"));
}

TEST(an_empty_net_log_points_at_the_allowlist_and_the_probe) {
  const std::vector<std::string> lines =
      tui::log_empty_lines(tui::LogFile::kNet);
  CHECK(contains(lines, "no outbound"));
  CHECK(contains(lines, "--show-endpoints"));
  CHECK(contains(lines, "__egress-probe"));
}

TEST(a_log_that_could_not_be_read_is_not_shown_as_an_empty_one) {
  // §11: the difference between "nothing happened" and "we cannot tell" is the
  // difference between a fact and a guess.
  const std::vector<std::string> lines = tui::log_error_lines(
      tui::LogFile::kDeploy, "tail: opening /x failed: Permission denied");
  CHECK(contains(lines, "could not be read"));
  CHECK(contains(lines, "Permission denied"));
  CHECK(contains(lines, "NOT the same as an empty file"));
}