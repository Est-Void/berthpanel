#pragma once

// Non-blocking TCP to a resolved address; waits honour deadline and cancel.

#include <atomic>
#include <chrono>
#include <string>

#include "util/result.h"

namespace berth {

[[nodiscard]] std::chrono::milliseconds remaining_ms(
    std::chrono::steady_clock::time_point deadline);

// In short slices, so a cancel is noticed within one slice.
[[nodiscard]] Result<void> wait_socket(
    int fd, short events, std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel, const std::string& what);

// Numeric address only (no resolver); Err carries the last failure.
[[nodiscard]] Result<int> tcp_connect(
    const std::string& ip, const std::string& port,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel);

}
