#include "netstatus.h"

#include <atomic>
#include <memory>
#include <string>

namespace berth {

namespace {

// total in the high 32 bits, allowed in the low. One word on purpose: separate
// counters let a reader observe a moment that never happened.
constexpr std::uint64_t kOne = 1;
constexpr std::uint64_t kTotalStep = kOne << 32;
constexpr std::uint64_t kAllowedMask = 0xffffffffULL;

struct Counters {
  std::atomic<std::uint64_t> totals{0};
  // atomic<shared_ptr>: std::atomic<std::string> does not exist in C++20, and a
  // mutex here would sit on the render thread.
  std::atomic<std::shared_ptr<const std::string>> last_host{};
};

Counters& counters() {
  static Counters instance;
  return instance;
}

std::string host_of(const Counters& c) {
  const std::shared_ptr<const std::string> host = c.last_host.load();
  return host ? *host : std::string();
}

}
void netstatus_record(const std::string_view host, const bool allowed) {
  Counters& c = counters();
  // 2^32 attempts in one session is unreachable; a wrapped banner is cosmetic,
  // not a security failure.
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
  // Read separately: last_host is a render hint, so it keeps the packed counter
  // word pointer-free.
  snapshot.last_host = host_of(c);
  return snapshot;
}

void netstatus_reset() {
  Counters& c = counters();
  c.totals.store(0, std::memory_order_relaxed);
  c.last_host.store(nullptr, std::memory_order_relaxed);
}

}
