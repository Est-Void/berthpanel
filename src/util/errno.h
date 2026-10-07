#pragma once

// src/util/errno.h — errno value to message text. Works with both the GNU
// strerror_r (char*) and the XSI one (int); callers just print the result.

#include <string.h>

#include <string>

namespace berth {

[[nodiscard]] inline std::string errno_text(int e) {
  char buf[128];
#if defined(__GLIBC__)
  return std::string(strerror_r(e, buf, sizeof(buf)));
#else
  const char* msg = "unknown error";
  if (strerror_r(e, buf, sizeof(buf)) == 0)
    msg = buf;
  return std::string(msg);
#endif
}

}  // namespace berth
