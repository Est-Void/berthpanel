// src/core/netstatus.cpp — see netstatus.h for the contract.

#include "netstatus.h"

#include <atomic>
#include <memory>
#include <string>

namespace berth {

namespace {

// total in the HIGH 32 bits, allowed in the LOW 32. One atomic word on purpose:
// three separate counters let a reader observe a moment that never existed
// (total incremented, allowed not yet), and a banner that prints a count that
// never happened is worse than no banner. denied is total - allowed, which
// makes the arithmetic identity exact by construction.
constexpr std::uint64_t kOne = 1;
constexpr std::uint64_t kTotalStep = kOne << 32;
constexpr std::uint64_t kAllowedMask = 0xffffffffULL;

struct Counters {
  std::atomic<std::uint64_t> totals{0};
  // atomic<shared_ptr>: an atomic STRING does not exist in C++20, and a mutex
  // here would be a mutex on the render thread.
  std::atomic<std::shared_ptr<const std::string>> last_host{};
};

// One per PROCESS (function-local static): the counters describe the session,
// not the file, so nothing here is on disk and nothing needs a lock.
Counters& counters() {
  static Counters instance;
  return instance;
}

std::string host_of(const Counters& c) {
  const std::shared_ptr<const std::string> host = c.last_host.load();
  return host ? *host : std::string();
}

}  // namespace

void netstatus_record(const std::string_view host, const bool allowed) {
  Counters& c = counters();
  // Overflow: 2^32 attempts (4.3 billion) in ONE panel session. Below the
  // lifetime of a single process for anything this binary does, and a wrapped
  // banner is a cosmetic lie, not a security failure — but it cannot happen
  // before the process dies of something else.
  c.totals.fetch_add(kTotalStep + (allowed ? kOne : 0),
                     std::memory_order_relaxed);
  c.last_host.store(std::make_shared<const std::string>(host),
                    std::memory_order_relaxed);
}

NetSnapshot netstatus_snapshot() {
  const Counters& c = counters();
  const std::uint64_t packed = c.totals.load(std::memory_order_relaxed);
  NetSnapshot snapshot;
  snapshot.total = packed >> 32;
  snapshot.allowed = packed & kAllowedMask;
  snapshot.denied = snapshot.total - snapshot.allowed;
  // Read separately from the packed word: a host name is a hint for the next
  // render, and forcing it into the same atomic would mean a heap pointer in a
  // counter that the render thread reads every frame.
  snapshot.last_host = host_of(c);
  return snapshot;
}

void netstatus_reset() {
  Counters& c = counters();
  c.totals.store(0, std::memory_order_relaxed);
  c.last_host.store(nullptr, std::memory_order_relaxed);
}

}  // namespace berth