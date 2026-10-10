#pragma once

// src/util/argv.h — inspection of an argv array. argv is the only currency
// for subprocesses (dev.md §11): nothing here splits or joins on whitespace.

#include <string>
#include <string_view>
#include <vector>

namespace berth {

// Basename of argv[0]: "/usr/bin/pnpm" -> "pnpm". Empty for an empty argv.
[[nodiscard]] inline std::string program_name(
    const std::vector<std::string>& argv) {
  if (argv.empty())
    return {};
  const std::string& head = argv.front();
  const std::size_t slash = head.rfind('/');
  return slash == std::string::npos ? head : head.substr(slash + 1);
}

[[nodiscard]] inline bool has_arg(const std::vector<std::string>& argv,
                                  const std::string_view flag) {
  for (const std::string& arg : argv) {
    if (arg == flag)
      return true;
  }
  return false;
}

}  // namespace berth
