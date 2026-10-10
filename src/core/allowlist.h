#pragma once

// src/core/allowlist.h — the egress policy table (dev.md §6.1). PURE: no
// sockets, no files, no logging. Every outbound request classifies its host
// here before the connect, and the table and tests/netlog_test.cpp change in
// the SAME commit with a public justification — the test pins the exact set,
// so a table edit cannot compile green unnoticed.

#include <string>
#include <string_view>
#include <vector>

namespace berth {

enum class EndpointClass {
  kAllowed,       // on the allowlist
  kLoopbackOnly,  // permitted for local health checks, never for API calls
  kDenied,
};

enum class RequestKind {
  kApi,          // anything that is not a local health check
  kHealthCheck,  // loopback probes of our own services
};

// Exact string match on the host (case-insensitive; [] stripped from IPv6
// literals). Never substring, never suffix — "api.github.com.evil.io" and
// "xapi.github.com" are denied (§6.3).
[[nodiscard]] EndpointClass classify_endpoint(std::string_view host);

// Is this request permitted at all? Loopback passes only for health checks;
// allowlisted hosts pass for API calls.
[[nodiscard]] bool endpoint_permitted(EndpointClass cls, RequestKind kind);

// The table itself, for --show-endpoints (§9).
[[nodiscard]] std::vector<std::string_view> allowed_endpoints();

}  // namespace berth
