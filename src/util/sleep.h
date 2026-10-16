#pragma once

#include <time.h>

#include <atomic>
#include <cerrno>
#include <chrono>

namespace berth {

// Resumes after EINTR with the remaining time, so a signal never shortens the
// wait.
inline void sleep_ms(const int ms) {
  struct timespec ts{static_cast<time_t>(ms / 1000),
                     static_cast<long>(ms % 1000) * 1000000L};
  while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

// Returns true only if the full budget elapsed; false means cancel cut it
// short.
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

}
