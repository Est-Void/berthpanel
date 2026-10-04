#pragma once

// src/netlog.h — kAllowedEndpoints and the net.log writer (dev.md §6.1).
// THE SECURITY TABLE: every outbound request passes classify_endpoint()
// before its socket connect, and every attempt — allowed, refused, failed —
// is appended to net.log. The allowlist and its test change in the SAME
// commit, with a public justification; the test pins the exact set, so a
// table change cannot compile green unnoticed. Owns no sockets.

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "paths.h"
#include "util/result.h"

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

// Policy: is this request permitted at all? Loopback passes only for
// health checks; allowlisted hosts pass for API calls.
[[nodiscard]] bool endpoint_permitted(EndpointClass cls, RequestKind kind);

// The table itself, for --show-endpoints (§9).
[[nodiscard]] std::vector<std::string_view> allowed_endpoints();

struct NetlogEntry {
  std::string_view host;
  std::string_view ip;      // connected/attempted address; may be empty
  std::uint16_t port = 0;
  std::string_view purpose;  // "https-get", "doh", "egress-probe", ...
  std::string_view note;     // outcome: status code, error, stop reason
  bool allowed = true;       // false marks a refused attempt (still logged!)
};

// Appends one ISO-8601 UTC line to net.log (0600). Thread-safe: a mutex
// plus a single O_APPEND write per entry. Newlines inside fields are
// sanitized so one entry is always one line. A failed log write is an
// error the caller must see — transparency is the whole point.
[[nodiscard]] Result<void> netlog_append(const Paths& paths,
                                         const NetlogEntry& entry);

}  // namespace berth
