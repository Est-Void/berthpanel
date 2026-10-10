#pragma once

// src/util/env.h — the only environment-lookup path in the codebase.
// getenv returning "" is NOT an unset value (dev.md §11): empty strings are
// filtered HERE, never at call sites. Tests mutate the environment only
// while holding env_test_lock() (dev.md §9) — setenv/unsetenv are not
// thread-safe and cannot be made so, only serialized.

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

namespace berth {

// Global mutex serializing every env-mutating test. Module-local locks do
// not protect across modules — always take this one.
inline std::mutex& env_test_lock() {
  static std::mutex m;
  return m;
}

// unset or empty -> nullopt; otherwise the value. Every lookup in the
// codebase goes through here.
[[nodiscard]] inline std::optional<std::string> env_nonempty(const char* name) {
  const char* v = ::getenv(name);
  if (v == nullptr || v[0] == '\0')
    return std::nullopt;
  return std::string(v);
}

}  // namespace berth
