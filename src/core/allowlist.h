#pragma once

// PURE: no sockets, no files, no logging.

#include <string>
#include <string_view>
#include <vector>

namespace berth {

enum class EndpointClass {
  kAllowed,
  kLoopbackOnly,  // health checks only, never API calls
  kDenied,
};

enum class RequestKind {
  kApi,
  kHealthCheck,  // probes of our own services
};

// Exact host match after lowercasing and stripping IPv6 brackets — never
// substring or suffix, so "xapi.github.com" stays denied.
[[nodiscard]] EndpointClass classify_endpoint(std::string_view host);

[[nodiscard]] bool endpoint_permitted(EndpointClass cls, RequestKind kind);

// The table itself, for --show-endpoints.
[[nodiscard]] std::vector<std::string_view> allowed_endpoints();

}
