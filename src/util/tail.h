#pragma once

// src/util/tail.h — read the LAST n lines of a file, bounded.
//
// Every log the panel shows (deploy.log, net.log, crash.log) is append-only
// and grows without limit, so the screen cannot read the whole thing: a net.log
// after a week's use is bigger than any panel should hold, and reading it per
// refresh is a blocking call on the render thread (§5). This reads a window
// from the END, which is the part a human is looking at.
//
// Two decisions worth stating, because both were open questions:
//
//   * A MISSING FILE IS NOT AN ERROR. An empty crash.log is the normal state —
//     "no crashes yet" is the good news — and a panel that renders an error box
//     over a file that has never been written is being dramatic about nothing.
//     So: ENOENT yields an empty result. Every OTHER failure (a directory, a
//     permission error, a read error) is an Err, because those are real and
//     silence would be a lie.
//   * The read is bounded by BYTES as well as lines. A file with no trailing
//     newline, or one enormous line, must not turn a 200-line tail into an
//     unbounded allocation: at most kTailWindowBytes are read, and if that
//     window starts mid-line the partial line is dropped rather than shown as a
//     truncated entry.

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

// The most bytes ever read from the end of a file. 64 KiB holds a few thousand
// short log lines; a deploy's tail is short lines.
inline constexpr std::size_t kTailWindowBytes = 64 * 1024;

// Splits on '\n' and drops a trailing empty field, so "a\nb\n" is two lines and
// "a\nb" is two lines. A line's own '\r' is left alone: the panel shows the
// file as written.
[[nodiscard]] std::vector<std::string> split_log_lines(std::string_view text);

// The last `wanted` lines of `path`. `wanted == 0` yields no lines but still
// reports whether the file exists.
[[nodiscard]] Result<Tail> tail_lines(const std::filesystem::path& path,
                                      std::size_t wanted);

// The pure half, exposed so the windowing is testable without a filesystem.
// `window_started_mid_line` says the text begins inside a line — the caller
// read from the middle of a file — and that fragment is dropped rather than
// shown as a truncated entry.
[[nodiscard]] Tail tail_from_text(std::string_view text, std::size_t wanted,
                                  bool window_started_mid_line = false);

}  // namespace berth