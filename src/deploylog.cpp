#include "deploylog.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "atomic.h"
#include "util/errno.h"
#include "util/fd.h"

namespace berth {

std::string deploylog_timestamp() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  tm tm_utc{};
  ::gmtime_r(&ts.tv_sec, &tm_utc);
  char buf[48];
  const std::size_t n = ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
  const int written = ::snprintf(buf + n, sizeof(buf) - n, ".%03ldZ",
                                 static_cast<long>(ts.tv_nsec / 1000000L));
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buf) - n) {
    return "unknown-time";
  }
  return std::string(buf, n + static_cast<std::size_t>(written));
}

Result<void> deploylog_append(const Paths& paths, const std::string_view line) {
  std::string text = deploylog_timestamp();
  text += " ";
  for (const char c : line) {
    text += (c == '\n' || c == '\r') ? ' ' : c;
  }
  text += "\n";

  for (int attempt = 0; attempt < 2; ++attempt) {
    FdGuard fd(::open(paths.deploy_log.c_str(),
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                      S_IRUSR | S_IWUSR));
    if (fd.get() >= 0) {
      std::size_t left = text.size();
      const char* cursor = text.data();
      while (left > 0) {
        const ssize_t n = ::write(fd.get(), cursor, left);
        if (n < 0) {
          if (errno == EINTR) continue;
          return Error("deploylog: write: " + errno_text(errno));
        }
        cursor += n;
        left -= static_cast<std::size_t>(n);
      }
      return {};
    }
    if (errno != ENOENT || attempt == 1) {
      return Error("deploylog: open " + paths.deploy_log.string() + ": " +
                   errno_text(errno));
    }
    Result<void> dir = ensure_dir(paths.state_dir);
    if (dir.is_err()) return dir;
  }
  return Error("deploylog: unreachable");
}

}  // namespace berth