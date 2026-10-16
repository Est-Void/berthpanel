#pragma once

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

namespace berth {

// setenv/unsetenv are not thread-safe and cannot be made so; module-local
// locks do not protect across modules, so always take this one.
inline std::mutex& env_test_lock() {
  static std::mutex m;
  return m;
}

// unset or empty -> nullopt (§11: empty means unset). The only env lookup in
// the codebase, so nothing filters empties at call sites.
[[nodiscard]] inline std::optional<std::string> env_nonempty(const char* name) {
  const char* v = ::getenv(name);
  if (v == nullptr || v[0] == '\0')
    return std::nullopt;
  return std::string(v);
}

}
