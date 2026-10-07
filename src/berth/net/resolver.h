#pragma once

// src/berth/net/resolver.h — host → addresses. PURE with respect to the
// network stack: this module speaks getaddrinfo and nothing else. It knows
// nothing about HTTP.
//
// The DoH fallback (dev.md §6: cloudflare-dns.com on pinned anycast IPs,
// logged like any other hop) is INJECTED as a callback. That is what breaks
// the resolver↔client dependency cycle: doh.cpp needs http_get, and
// http_get needs resolve_host, so one of them has to be given the other
// rather than call it. fetch.h wires the real pair together.
//
// getaddrinfo cannot be interrupted, so it runs on a helper thread that is
// detached on timeout (glibc caps its own retries) — the caller's error is
// prompt either way (§5).

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

// Given a host, addresses, or a loud error.
using DohFallback = std::function<Result<std::vector<ResolvedIp>>(
    const std::string& host, std::chrono::milliseconds budget,
    const Paths& paths, const std::atomic<bool>* cancel)>;

// System DNS first, then `doh_fallback` with whatever budget is left. Both
// hops net.log a "resolver" line, so a failure to reach the name server is
// visible in net.log rather than only in a return code.
[[nodiscard]] Result<std::vector<ResolvedIp>> resolve_host(
    const std::string& host, std::chrono::milliseconds deadline,
    const Paths& paths, const std::atomic<bool>* cancel,
    const DohFallback& doh_fallback);

}  // namespace berth
