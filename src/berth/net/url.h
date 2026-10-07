#pragma once

// src/berth/net/url.h — URL parsing and redirect-relative resolution. PURE:
// no sockets, no policy, no logging. This is the only place that decides
// what a URL's host, port and path ARE; the allowlist decision lives one
// layer up (core/allowlist.h) and is never taken from a substring here.

#include <cstdint>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

struct HttpUrl {
  std::string scheme;      // "https" (or "http" — loopback health checks only)
  std::string host;        // lowercased; brackets stripped for IPv6 literals
  std::string port;        // "443"/"80"/explicit
  std::string path_query;  // always starts with '/'

  // The port as a number, for sockets and net.log. Safe to call: parse_http_url
  // has already rejected anything outside 1..65535.
  [[nodiscard]] std::uint16_t port_number() const;
};

// http/https only, no userinfo, no control characters, no relative forms.
// A refusal comes back as Err with the "refused: " root marker, which the
// client's retry loop treats as permanent.
[[nodiscard]] Result<HttpUrl> parse_http_url(std::string_view url);

// RFC 3986 subset needed for redirect Locations: absolute URL,
// scheme-relative "//host/path", root-relative "/path", and a path relative
// to the current document. Returns "" for anything unresolvable.
[[nodiscard]] std::string join_redirect(std::string_view current_url,
                                        std::string_view location);

}  // namespace berth
