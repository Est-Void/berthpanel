#pragma once

// Pure, and the only place a URL's host, port and path are decided: the
// allowlist decision lives one layer up and is never a substring here.

#include <cstdint>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

struct HttpUrl {
  std::string scheme;  // "https" (or "http" — loopback health checks only)
  std::string host;   // lowercased; brackets stripped for IPv6 literals
  std::string port;
  std::string path_query;  // always starts with '/'

  // Safe to call: parse_http_url already rejected anything outside 1..65535.
  [[nodiscard]] std::uint16_t port_number() const;
};

// http/https only; no userinfo, no control characters, no relative forms.
// Refusals carry the "refused: " root marker.
[[nodiscard]] Result<HttpUrl> parse_http_url(std::string_view url);

// Absolute, "//host/path", "/path" or document-relative; "" when unresolvable.
[[nodiscard]] std::string join_redirect(std::string_view current_url,
                                        std::string_view location);

}
