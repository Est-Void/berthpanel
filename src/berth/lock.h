#pragma once

// flock(LOCK_EX|LOCK_NB): the kernel drops it on any exit, SIGKILL and power
// loss included — that is what makes "entry present AND lock free" safe.

#include <string>

#include "../core/paths.h"
#include "../util/fd.h"
#include "../util/result.h"

namespace berth {

class DeployLock {
 public:
  DeployLock() = default;
  DeployLock(DeployLock&&) = default;
  DeployLock& operator=(DeployLock&&) = default;
  DeployLock(const DeployLock&) = delete;
  DeployLock& operator=(const DeployLock&) = delete;

  // Err says "already running" or a real I/O failure; the two differ.
  [[nodiscard]] static Result<DeployLock> acquire(const Paths& paths);

  [[nodiscard]] bool held() const { return fd_.get() >= 0; }

 private:
  explicit DeployLock(FdGuard fd) : fd_(std::move(fd)) {}
  FdGuard fd_;
};

// Test-only SIGKILL scenario hook (`__test-lock-child`), not a user command.
int lock_test_child(const char* lock_path);

}
