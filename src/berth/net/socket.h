#pragma once

// src/berth/net/socket.h — non-blocking TCP to an ALREADY-RESOLVED address.
// No name resolution here and no TLS: those are the two layers above, and
// keeping them out is what lets this file be reasoned about on its own.
// Every wait honours a deadline and the cancellation flag (§5).

#include <atomic>
#include <chrono>
#include <string>

#include "util/result.h"

namespace berth {

// Milliseconds left until `deadline`, floored at zero.
[[nodiscard]] std::chrono::milliseconds remaining_ms(
    std::chrono::steady_clock::time_point deadline);

// Waits for `events` on `fd`, in short slices so a cancel is noticed within
// one slice. Err on cancel, timeout or poll failure.
[[nodiscard]] Result<void> wait_socket(
    int fd, short events, std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel, const std::string& what);

// Connects to a NUMERIC address (no resolver runs here). Tries every address
// getaddrinfo(AI_NUMERICHOST) offers and returns the fd of the first that
// connects; Err carries the last failure. The caller owns the returned fd.
[[nodiscard]] Result<int> tcp_connect(
    const std::string& ip, const std::string& port,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel);

}  // namespace berth
