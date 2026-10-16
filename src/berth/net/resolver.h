#pragma once

// The DoH fallback is injected: it and the client would otherwise be a cycle.

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "../../core/paths.h"
#include "util/result.h"

namespace berth {

struct ResolvedIp {
  std::string ip;  // numeric form, usable with AI_NUMERICHOST connects
  bool ipv6 = false;
};

using DohFallback = std::function<Result<std::vector<ResolvedIp>>(
    const std::string& host, std::chrono::milliseconds budget,
    const Paths& paths, const std::atomic<bool>* cancel)>;

// Both hops net.log a resolver line, so a DNS failure is visible.
[[nodiscard]] Result<std::vector<ResolvedIp>> resolve_host(
    const std::string& host, std::chrono::milliseconds deadline,
    const Paths& paths, const std::atomic<bool>* cancel,
    const DohFallback& doh_fallback);

}
