#include "applog.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "util/errno.h"
#include "util/fd.h"
#include "util/fs.h"

namespace berth {

std::string log_timestamp() {
  struct timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_utc{};
  ::gmtime_r(&ts.tv_sec, &tm_utc);
  char buf[48];
  const std::size_t n =
      ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
  const int written = ::snprintf(buf + n, sizeof(buf) - n, ".%03ldZ",
                                 static_cast<long>(ts.tv_nsec / 1000000L));
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buf) - n) {
    return "unknown-time";
  }
  return std::string(buf, n + static_cast<std::size_t>(written));
}

std::string one_line(const std::string_view field) {
  std::string out;
  out.reserve(field.size());
  for (const char c : field) {
    out += (c == '\n' || c == '\r') ? ' ' : c;
  }
  return out;
}

Result<void> append_log_line(const std::filesystem::path& file,
                             const std::filesystem::path& dir,
                             const std::string_view line) {
  // One retry: the first ENOENT means the XDG state dir is missing — fix it
  // rather than report it.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const int raw =
        ::open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
               S_IRUSR | S_IWUSR);
    if (raw >= 0) {
      const FdGuard fd(raw);
      return write_all(fd.get(), line);
    }
    const int saved = errno;
    if (saved != ENOENT || attempt == 1) {
      return Error("applog: open " + file.string() + ": " + errno_text(saved));
    }
    const Result<void> made = ensure_dir(dir);
    if (made.is_err())
      return made;
  }
  return Error("applog: unreachable");
}

}
