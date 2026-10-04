#pragma once

// src/berth/resolver.h — host resolution: system getaddrinfo first, DoH
// fallback second (dev.md §6: cloudflare-dns.com on pinned anycast IPs,
// logged like any other hop). Both hops are bounded by the deadline and
// honour the cancellation flag. The DoH JSON parser is pure and tested
// offline; live resolution is opt-in (BERTHPANEL_RUN_NETWORK_TESTS).

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "../paths.h"
#include "../util/result.h"

namespace berth {

struct ResolvedIp {
  std::string ip;  // numeric form, usable with AI_NUMERICHOST connects
  bool ipv6 = false;
};

// Parses a DoH (RFC 8484 application/dns-json) body into A/AAAA records.
// Records that fail inet_pton validation are skipped; a response with no
// usable address at all is a loud error, never an empty success.
[[nodiscard]] Result<std::vector<ResolvedIp>> parse_doh_answers(
    std::string_view body);

// resolve = system DNS (budget-capped) → DoH fallback (remaining budget).
// The system resolver runs on a helper thread: getaddrinfo cannot be
// interrupted, so on timeout the thread is detached (glibc caps its own
// retry time) and the error returns immediately.
[[nodiscard]] Result<std::vector<ResolvedIp>> resolve_host(
    const std::string& host, std::chrono::milliseconds deadline,
    const Paths& paths, std::atomic<bool>* cancel);

}  // namespace berth
