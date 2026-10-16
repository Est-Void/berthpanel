#pragma once

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "util/errno.h"
#include "util/result.h"

namespace berth {

class FdGuard {
 public:
  FdGuard() = default;
  explicit FdGuard(int fd) : fd_(fd) {}
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  FdGuard(FdGuard&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  FdGuard& operator=(FdGuard&& other) noexcept {
    if (this != &other) {
      close_now();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~FdGuard() { close_now(); }

  [[nodiscard]] int get() const { return fd_; }

  // Closes now (e.g. before rename, so no fd lingers open on the temp).
  void close_now() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // Gives up ownership without closing.
  [[nodiscard]] int release() { return std::exchange(fd_, -1); }

 private:
  int fd_ = -1;
};

// A partial write to an append-only log is silent data loss; the loop makes
// it impossible and reports only a hard failure.
[[nodiscard]] inline Result<void> write_all(const int fd,
                                            const std::string_view data) {
  std::size_t left = data.size();
  const char* cursor = data.data();
  while (left > 0) {
    const ssize_t n = ::write(fd, cursor, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return Error("write: " + std::string(errno_text(errno)));
    }
    cursor += n;
    left -= static_cast<std::size_t>(n);
  }
  return {};
}

}
