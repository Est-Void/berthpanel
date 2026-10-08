// Tests for util/tail.h (stage 6.6). The panel shows the tail of three
// append-only logs, so the properties that matter are: a file shorter than the
// request comes back whole, a longer one comes back with its NEWEST lines, and
// an ABSENT file is an empty result rather than an error — an empty crash.log
// is the good state, not a failure.

#include "util/tail.h"

#include <string>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

namespace {

std::string numbered_lines(const std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) {
    out += "line " + std::to_string(i) + "\n";
  }
  return out;
}

}  // namespace

TEST(tail_of_a_missing_file_is_empty_and_not_an_error) {
  // The documented decision: ENOENT is the "no log yet" state. An error box
  // over a crash.log that has never existed would be drama about nothing.
  const TempDir tmp;
  const auto tail = berth::tail_lines(tmp.path() / "crash.log", 10);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.empty());
  CHECK(tail.value().existed == false);
  CHECK(!tail.value().truncated);
}

TEST(tail_of_an_empty_file_is_empty_and_existed) {
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "net.log";
  CHECK(berth::atomic_write(path, "").is_ok());
  const auto tail = berth::tail_lines(path, 10);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.empty());
  CHECK(tail.value().existed);
}

TEST(tail_shorter_than_n_returns_every_line) {
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "deploy.log";
  CHECK(berth::atomic_write(path, numbered_lines(3)).is_ok());
  const auto tail = berth::tail_lines(path, 10);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.size() == 3);
  CHECK(tail.value().lines.front() == "line 0");
  CHECK(tail.value().lines.back() == "line 2");
  CHECK(!tail.value().truncated);
}

TEST(tail_of_exactly_n_lines_returns_all_of_them) {
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "deploy.log";
  CHECK(berth::atomic_write(path, numbered_lines(5)).is_ok());
  const auto tail = berth::tail_lines(path, 5);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.size() == 5);
  CHECK(tail.value().lines.front() == "line 0");
}

TEST(tail_longer_than_n_returns_the_newest_lines_in_order) {
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "deploy.log";
  CHECK(berth::atomic_write(path, numbered_lines(100)).is_ok());
  const auto tail = berth::tail_lines(path, 4);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.size() == 4);
  // The NEWEST, oldest-first: a log you cannot see the end of is useless.
  CHECK(tail.value().lines[0] == "line 96");
  CHECK(tail.value().lines[3] == "line 99");
}

TEST(tail_a_file_without_a_trailing_newline_keeps_its_last_line) {
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "crash.log";
  CHECK(berth::atomic_write(path, "segv in worker\nlast line, no newline")
            .is_ok());
  const auto tail = berth::tail_lines(path, 10);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.size() == 2);
  CHECK(tail.value().lines.back() == "last line, no newline");
}

TEST(tail_wanting_zero_lines_still_reports_existence) {
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "net.log";
  CHECK(berth::atomic_write(path, numbered_lines(10)).is_ok());
  const auto tail = berth::tail_lines(path, 0);
  CHECK(tail.is_ok());
  CHECK(tail.value().lines.empty());
  CHECK(tail.value().existed);
}

TEST(tail_refuses_to_read_a_directory_as_a_log) {
  // A directory where a log should be is a real fault, and reporting it as
  // "no lines yet" would be the quiet wrong answer.
  const TempDir tmp;
  const std::filesystem::path dir = tmp.path() / "net.log";
  CHECK(berth::ensure_dir(dir).is_ok());
  const auto tail = berth::tail_lines(dir, 10);
  CHECK(tail.is_err());
  CHECK(tail.error().ctx().find("not a regular file") != std::string::npos);
}

TEST(tail_beyond_the_byte_window_reports_the_truncation) {
  // A log bigger than the window must SAY so rather than quietly showing a
  // fragment that reads like the whole file.
  const TempDir tmp;
  const std::filesystem::path path = tmp.path() / "net.log";
  std::string huge = "0.0.0.0:0000 allowed note=\"";
  huge.append(200, 'x');
  huge += "\"\n";
  for (int i = 0; i < 2000; ++i) {
    huge += "127.0.0.1:1F90 0.0.0.0:0000 purpose=https-get note=pad ";
    huge.append(200, 'y');
    huge += " end\n";
  }
  CHECK(berth::atomic_write(path, huge).is_ok());
  const auto tail = berth::tail_lines(path, 10);
  CHECK(tail.is_ok());
  CHECK(tail.value().truncated);
  CHECK(tail.value().lines.size() == 10);
  // The oldest kept line is a WHOLE line: the window's first fragment was
  // dropped rather than shown as half an entry.
  CHECK(tail.value().lines.front().rfind("127.0.0.1:1F90", 0) == 0);
  CHECK(tail.value().lines.back().size() > 200);
}

TEST(tail_split_keeps_empty_lines_and_drops_only_the_trailing_one) {
  const std::vector<std::string> lines = berth::split_log_lines("a\n\nb\n");
  // An empty line IS a line — dropping blank entries would silently
  // de-duplicate whatever the log is recording.
  CHECK(lines.size() == 3);
  CHECK(lines[0] == "a");
  CHECK(lines[1].empty());
  CHECK(lines[2] == "b");

  const std::vector<std::string> unterminated =
      berth::split_log_lines("a\n\nb");
  CHECK(unterminated.size() == 3);
  CHECK(unterminated[2] == "b");

  CHECK(berth::split_log_lines("").empty());
}

TEST(tail_from_text_takes_the_tail_without_a_file) {
  // The pure half, so the windowing is testable on its own.
  const berth::Tail tail = berth::tail_from_text("a\nb\nc\nd\n", 2);
  CHECK(tail.lines.size() == 2);
  CHECK(tail.lines[0] == "c");
  CHECK(tail.lines[1] == "d");

  const berth::Tail all = berth::tail_from_text("a\nb\n", 10);
  CHECK(all.lines.size() == 2);
  CHECK(!all.truncated);

  // A window that starts inside a line drops the fragment, THEN keeps the
  // newest `wanted` — doing it the other way round silently loses a line.
  const berth::Tail windowed =
      berth::tail_from_text("ragment\nb\nc\nd\n", 2, true);
  CHECK(windowed.lines.size() == 2);
  CHECK(windowed.lines[0] == "c");
  CHECK(windowed.lines[1] == "d");
}