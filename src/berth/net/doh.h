#pragma once

// src/berth/net/doh.h — DNS-over-HTTPS fallback (dev.md §6): cloudflare-dns.com
// reached over its PINNED anycast IPs, so the fallback cannot be defeated by
// breaking the name server it would otherwise need. It sits ABOVE the client
// (it is an ordinary allowlisted HTTPS GET) and is what fetch.h injects into
// resolve_host.
//
// The JSON answer parser is pure and tested offline; the live fetch is
// opt-in (BERTHPANEL_RUN_NETWORK_TESTS).

#include <atomic>
#include <chrono>
#include <string_view>
#include <vector>

#include "../../core/paths.h"
#include "resolver.h"
#include "util/result.h"

namespace berth {

// Parses a DoH (RFC 8484 application/dns-json) body into A/AAAA records.
// Records failing inet_pton validation are skipped; a response with no
// usable address at all is a loud error, never an empty success.
[[nodiscard]] Result<std::vector<ResolvedIp>> parse_doh_answers(
    std::string_view body);

// The DohFallback to hand to resolve_host. Tries each pinned bootstrap IP in
// turn, so one anycast address being unreachable is not a total failure.
[[nodiscard]] DohFallback doh_fallback();

}  // namespace berth
