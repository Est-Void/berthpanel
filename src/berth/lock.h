#pragma once

// src/berth/lock.h — the deploy mutex: flock(LOCK_EX|LOCK_NB) on
// paths.deploy_lock (dev.md §6.7). The kernel drops the lock on any exit
// — SIGKILL and power loss included — so "journal entry present AND lock
// free" can never race a live deploy. That is the whole cleanup design.

#include <string>

#include "../paths.h"
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

  // Err when another deploy holds the lock ("already running"), or on
  // real I/O failure — the message distinguishes the two.
  [[nodiscard]] static Result<DeployLock> acquire(const Paths& paths);

  [[nodiscard]] bool held() const { return fd_.get() >= 0; }

 private:
  explicit DeployLock(FdGuard fd) : fd_(std::move(fd)) {}
  FdGuard fd_;
};

// Test-only hook for the SIGKILL scenario (stage 4.3): takes the lock on
// `lock_path`, then pauses until killed. Wired from main.cpp as
// `__test-lock-child`; not a user command.
int lock_test_child(const char* lock_path);

}  // namespace berth
