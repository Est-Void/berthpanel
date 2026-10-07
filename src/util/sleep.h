#pragma once

// src/util/sleep.h — the only sleeps in the tree. Every wait is a deadline
// plus a cancel flag (dev.md §5): nothing blocks long enough for a Ctrl-C
// to look like a hang.

#include <time.h>

#include <atomic>
#include <cerrno>
#include <chrono>

namespace berth {

// One uninterruptible-by-policy slice. EINTR is resumed with the remaining
// time, so a signal never shortens the wait.
inline void sleep_ms(const int ms) {
  struct timespec ts{static_cast<time_t>(ms / 1000),
                     static_cast<long>(ms % 1000) * 1000000L};
  while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

// Sleeps up to budget, giving up as soon as cancel is set. Returns true when
// the full budget elapsed, false when it was cut short.
inline bool interruptible_sleep(
    const std::chrono::milliseconds budget,
    const std::atomic<bool>* cancel = nullptr,
    const std::chrono::milliseconds slice = std::chrono::milliseconds(50)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    sleep_ms(static_cast<int>((left < slice ? left : slice).count()));
  }
  return true;
}

}  // namespace berth
