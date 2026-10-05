#pragma once

// src/berth/httpclient.h — the allowlisted HTTPS client (dev.md §6.1–6.3).
//
// Invariants owned here:
//   §6.1  every outbound request is net.logged, allowed or refused.
//   §6.2  the allowlist gate runs before EVERY socket connect; redirects
//         are never auto-followed — next_redirect() decides, and the loop
//         it serves is bounded by kMaxRedirectHops. The decision function
//         is pure and its test exercises the cap boundary for real.
//   §6.3  a bearer token is sent ONLY when the request host is exactly
//         "api.github.com". Never in logs, never in argv.
//   §11   a complete HTTP response is a success at ANY status: a 403 is
//         not a network outage. Err is transport-level failure only.
//
// No proxy env, no system CA path (trust store is embedded, §2). Owns the
// TLS session lifecycle for one request; sockets are non-blocking and
// every wait has a deadline and honours the cancellation flag.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../netlog.h"
#include "../paths.h"
#include "../util/result.h"

namespace berth {

// Hard ceiling; HttpGetOptions cannot request more (a caller asking for
// more than the cap is a bug, and the cap is what the §6.2 test pins).
inline constexpr int kMaxRedirectHops = 5;
inline constexpr std::size_t kMaxBodyBytes = 64ull * 1024 * 1024;
inline constexpr std::size_t kMaxHeadBytes = 64 * 1024;

struct HttpUrl {
  std::string scheme;      // "https" (or "http" — loopback health checks only)
  std::string host;        // lowercased; brackets stripped for IPv6 literals
  std::string port;        // "443"/"80"/explicit
  std::string path_query;  // always starts with '/'
};

[[nodiscard]] Result<HttpUrl> parse_http_url(std::string_view url);

// RFC 3986 subset needed for redirect Locations: absolute URL,
// scheme-relative "//host/path", root-relative "/path", and a path
// relative to the current document. Returns "" for anything unresolvable.
[[nodiscard]] std::string join_redirect(std::string_view current_url,
                                        std::string_view location);

struct HttpReply {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
  std::string body;
  int hops = 0;           // redirects followed to reach this reply
  std::string stop_note;  // why a redirect loop stopped before following

  // Case-insensitive header lookup; nullptr when absent.
  [[nodiscard]] const std::string* header(std::string_view name) const;
};

// Splits a wire response into head + parsed reply. Requires the full head
// ("\r\n\r\n") to be present. Does not touch the body.
[[nodiscard]] Result<std::pair<HttpReply, std::size_t>> parse_http_head(
    std::string_view wire);

enum class BodyMode {
  kContentLength,
  kChunked,
  kUntilClose,
};

// Selects the body framing from parsed headers (transfer-encoding wins).
[[nodiscard]] BodyMode body_mode_of(const HttpReply& reply);

// Returns the declared Content-Length, or nullopt when absent/invalid.
[[nodiscard]] std::optional<std::size_t> content_length_of(const HttpReply& reply);

// Decodes a chunked body. Returns nullopt while the input is incomplete;
// Err on a malformed chunk grammar or an oversized total.
[[nodiscard]] Result<std::optional<std::string>> decode_chunked(
    std::string_view body, std::size_t max_bytes);

// True when the response accumulated so far is complete under `mode`.
[[nodiscard]] bool response_is_complete(const std::string& wire,
                                        std::size_t head_len, BodyMode mode,
                                        std::size_t content_length);

struct HttpGetOptions {
  RequestKind kind = RequestKind::kApi;
  std::string purpose = "https-get";  // net.log purpose field
  int max_redirect_hops = 0;          // 0 = never follow; capped by
                                      // kMaxRedirectHops (excess = Err)
  // Explicit opt-in for callers that KNOW their upstream redirects
  // (GitHub release assets 302 to release-assets.githubusercontent.com,
  // which is on the allowlist for exactly that reason). Every hop is still
  // gated per hop; a redirect to a non-allowlisted host is refused.
  bool follow_allowlisted_redirects = false;
  std::chrono::milliseconds per_attempt_timeout{30000};
  int max_attempts = 3;               // transport failures only
  std::size_t max_body_bytes = kMaxBodyBytes;
  const std::string* bearer_token = nullptr;  // §6.3: api.github.com only
  const std::string* pinned_ip = nullptr;     // skip resolver (DoH bootstrap)
  std::atomic<bool>* cancel = nullptr;
};

enum class RedirectAction {
  kNotRedirect,     // not 3xx, or no redirect loop should run
  kFollow,          // target parsed, within cap, and allowlisted (§6.2)
  kStopCap,         // hop budget exhausted: return the redirect as-is
  kStopDenied,      // redirect target refused by the allowlist
  kStopBadLocation, // missing or unresolvable Location
};

struct RedirectDecision {
  RedirectAction action = RedirectAction::kNotRedirect;
  std::string next_url;
};

// The hop budget a request actually gets: 0 unless the caller opted in.
[[nodiscard]] int effective_redirect_budget(const HttpGetOptions& options);

// The pure redirect-loop driver. `hops_done` is the number of redirects
// already followed for this logical request. A target is followable only
// when hops_done < the effective budget (options capped by
// kMaxRedirectHops) AND the target host passes the same allowlist gate as
// an initial request.
[[nodiscard]] RedirectDecision next_redirect(const HttpReply& reply,
                                             const HttpGetOptions& options,
                                             const std::string& current_url,
                                             int hops_done);

// Performs one logical GET: per-hop allowlist gating, resolution (unless
// pinned_ip), TLS, retries on transport failure (bounded, cancel-aware,
// every attempt net.logged), and the bounded non-auto redirect loop.
[[nodiscard]] Result<HttpReply> http_get(const std::string& url,
                                         const HttpGetOptions& options,
                                         const Paths& paths);

}  // namespace berth
