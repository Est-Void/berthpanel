#pragma once

// Delimiters are always explicit: splitting or joining on whitespace is
// banned project-wide (§11), spaces in paths are why.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace berth {

namespace detail {

inline void abort_empty_delimiter() {
  std::fputs("berth: invariant violated: strutil delimiter is empty\n", stderr);
  std::abort();
}

// Keeps empty fields (split("a,,b", ",") -> {"a", "", "b"}) and never trims:
// whitespace is data, not a delimiter.
}
inline std::vector<std::string> split(std::string_view text,
                                      std::string_view delim) {
  if (delim.empty())
    detail::abort_empty_delimiter();
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (true) {
    const std::size_t next = text.find(delim, pos);
    if (next == std::string_view::npos) {
      parts.emplace_back(text.substr(pos));
      return parts;
    }
    parts.emplace_back(text.substr(pos, next - pos));
    pos = next + delim.size();
  }
}

// Inverse of split(): join(split(x, d), d) == x.
inline std::string join(const std::vector<std::string>& parts,
                        std::string_view delim) {
  if (delim.empty())
    detail::abort_empty_delimiter();
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0)
      out += delim;
    out += parts[i];
  }
  return out;
}

// Whitespace inside is preserved.
inline std::string_view trim(std::string_view s) {
  constexpr std::string_view kWhitespace = " \t\n\v\f\r";
  const std::size_t begin = s.find_first_not_of(kWhitespace);
  if (begin == std::string_view::npos)
    return {};
  const std::size_t end = s.find_last_not_of(kWhitespace);
  return s.substr(begin, end - begin + 1);
}

// URLs need an exact host match, never a substring (§6.3) — these are the
// tool for that, `find` is not.
[[nodiscard]] inline bool starts_with(std::string_view s,
                                      std::string_view prefix) {
  return s.starts_with(prefix);
}

[[nodiscard]] inline bool ends_with(std::string_view s,
                                    std::string_view suffix) {
  return s.ends_with(suffix);
}

}
