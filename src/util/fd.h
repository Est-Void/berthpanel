#pragma once

// src/util/fd.h — RAII owner for a single POSIX file descriptor
// (dev.md §4: every handle gets a scoped wrapper). Move-only, closes on
// destruction, never owns more than one fd. Subprocesses, sockets and
// mbedTLS contexts get their own wrappers.

#include <unistd.h>

#include <utility>

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

}  // namespace berth
