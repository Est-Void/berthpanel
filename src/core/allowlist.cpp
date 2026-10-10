// src/core/allowlist.cpp — see allowlist.h for the contract.

#include "allowlist.h"

#include <array>

#include "util/ascii.h"

namespace berth {

namespace {

// THE SECURITY TABLE (dev.md §6). Every addition or removal lands in the
// same commit as a matching change to tests/netlog_test.cpp, with a public
// justification in the commit message.
constexpr std::array<std::string_view, 10> kAllowedEndpoints = {
    "api.github.com",
    "cdn.jsdelivr.net",
    "cloudflare-dns.com",
    "codeload.github.com",
    "dl.google.com",
    "github.com",
    "proxy.golang.org",
    "raw.githubusercontent.com",
    "release-assets.githubusercontent.com",
    "storage.googleapis.com",
};

constexpr std::array<std::string_view, 3> kLoopbackHosts = {
    "127.0.0.1",
    "::1",
    "localhost",
};

// Lowercase, without the brackets of an IPv6 literal.
std::string normalize_host(const std::string_view host) {
  std::string out;
  out.reserve(host.size());
  for (const char c : host) {
    if (c == '[' || c == ']')
      continue;
    out += (is_upper_alpha(c)) ? static_cast<char>(c - 'A' + 'a') : c;
  }
  return out;
}

}  // namespace

std::vector<std::string_view> allowed_endpoints() {
  return {kAllowedEndpoints.begin(), kAllowedEndpoints.end()};
}

EndpointClass classify_endpoint(const std::string_view host) {
  if (host.empty())
    return EndpointClass::kDenied;
  const std::string normalized = normalize_host(host);
  for (const std::string_view allowed : kAllowedEndpoints) {
    if (normalized == allowed)
      return EndpointClass::kAllowed;
  }
  for (const std::string_view loopback : kLoopbackHosts) {
    if (normalized == loopback)
      return EndpointClass::kLoopbackOnly;
  }
  return EndpointClass::kDenied;
}

bool endpoint_permitted(const EndpointClass cls, const RequestKind kind) {
  switch (cls) {
    case EndpointClass::kAllowed:
      return true;
    case EndpointClass::kLoopbackOnly:
      return kind == RequestKind::kHealthCheck;
    case EndpointClass::kDenied:
      return false;
  }
  return false;
}

}  // namespace berth
