// src/util/tail.cpp — see tail.h for the contract.

#include "tail.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>

#include "util/errno.h"
#include "util/strutil.h"

namespace berth {

namespace {

// Reads at most kTailWindowBytes from `offset` to the end. Short reads are not
// errors: a log is appended to concurrently and a partial read is simply an
// older view of it.
Result<std::string> read_window(const std::filesystem::path& path,
                                std::size_t offset, std::size_t budget) {
  std::FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) {
    return Error("tail: opening " + path.string() +
                 " failed: " + errno_text(errno));
  }
  if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
    const int saved = errno;
    std::fclose(file);
    return Error("tail: seeking in " + path.string() +
                 " failed: " + errno_text(saved));
  }
  std::string out;
  out.resize(budget);
  const std::size_t got = std::fread(out.data(), 1, budget, file);
  const bool failed = std::ferror(file) != 0;
  const int saved = errno;
  std::fclose(file);
  if (failed) {
    return Error("tail: reading " + path.string() +
                 " failed: " + errno_text(saved));
  }
  out.resize(got);
  return out;
}

}  // namespace

std::vector<std::string> split_log_lines(const std::string_view text) {
  std::vector<std::string> lines = berth::split(text, "\n");
  // Every append ends with "\n", so the split leaves one empty field at the
  // end. A file whose last line has no newline must not lose that line.
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

Tail tail_from_text(const std::string_view text, const std::size_t wanted,
                    const bool window_started_mid_line) {
  Tail tail;
  std::vector<std::string> lines = split_log_lines(text);
  if (window_started_mid_line && !lines.empty()) {
    // The window begins inside a line, so its first entry is a fragment.
    // Dropping it beats showing half a log entry as if it were whole.
    lines.erase(lines.begin());
  }
  tail.truncated = window_started_mid_line;
  if (lines.size() > wanted) {
    // wanted < lines.size(), so this cannot underflow the iterator.
    tail.lines.assign(lines.end() - static_cast<std::ptrdiff_t>(wanted),
                      lines.end());
  } else {
    tail.lines = std::move(lines);
  }
  return tail;
}

Result<Tail> tail_lines(const std::filesystem::path& path,
                        const std::size_t wanted) {
  Tail tail;
  // ENOENT is the documented "no log yet" state, not a failure — see the
  // header. Any other stat failure is real.
  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    if (ec && ec != std::errc::no_such_file_or_directory) {
      return Error("tail: cannot stat " + path.string() + ": " + ec.message());
    }
    return tail;  // exists == false, lines == {}
  }
  if (!std::filesystem::is_regular_file(status)) {
    return Error("tail: " + path.string() +
                 " is not a regular file — refusing to read a directory or a "
                 "device as a log");
  }

  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return Error("tail: cannot size " + path.string() + ": " + ec.message());
  }
  tail.existed = true;

  const std::size_t from =
      size > static_cast<std::uintmax_t>(kTailWindowBytes)
          ? static_cast<std::size_t>(size) - kTailWindowBytes
          : 0;
  tail.truncated = from > 0;
  Result<std::string> window = read_window(path, from, kTailWindowBytes);
  if (window.is_err()) {
    return std::move(window).error();
  }

  // One pass over the window: drop the fragment the window starts inside, then
  // keep the newest `wanted`.
  Tail parsed = tail_from_text(window.value(), wanted, tail.truncated);
  parsed.existed = true;
  return parsed;
}

}  // namespace berth