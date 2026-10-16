#pragma once

// <cctype> is deliberately unused: its answers are locale-dependent, and
// every caller here validates untrusted bytes.

#include <string_view>

namespace berth {

[[nodiscard]] constexpr bool is_digit(const char c) {
  return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_lower_hex(const char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f');
}

[[nodiscard]] constexpr bool is_hex(const char c) {
  return is_lower_hex(c) || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr bool is_upper_alpha(const char c) {
  return c >= 'A' && c <= 'Z';
}

[[nodiscard]] constexpr bool is_lower_alpha(const char c) {
  return c >= 'a' && c <= 'z';
}

[[nodiscard]] constexpr bool is_alpha(const char c) {
  return is_upper_alpha(c) || is_lower_alpha(c);
}

[[nodiscard]] constexpr bool is_alnum(const char c) {
  return is_alpha(c) || is_digit(c);
}

// github.com owner and repository names (§6).
[[nodiscard]] constexpr bool is_repo_char(const char c) {
  return is_alnum(c) || c == '-' || c == '.' || c == '_' || c == '/';
}

// '@' stays legal inside a unit stem: it is what separates the instance name.
[[nodiscard]] constexpr bool is_unit_stem_char(const char c) {
  return is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '@';
}

// EnvironmentFile key: POSIX-portable identifier, no leading digit.
[[nodiscard]] constexpr bool is_env_key_first(const char c) {
  return is_alpha(c) || c == '_';
}

[[nodiscard]] constexpr bool is_env_key_char(const char c) {
  return is_alnum(c) || c == '_';
}

template <typename Pred>
[[nodiscard]] constexpr bool all_chars(const std::string_view s,
                                       const Pred pred) {
  for (const char c : s) {
    if (!pred(c))
      return false;
  }
  return true;
}

}
