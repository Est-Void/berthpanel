#pragma once

// src/berth/net/protocol.h — HTTP/1.1 wire-format logic. PURE: it takes
// bytes and returns parsed values, touches no socket and consults no
// policy except the allowlist call the redirect decision needs (dev.md
// §6.2). Everything here is unit-testable offline, which is the point of
// keeping it out of the transport.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/allowlist.h"
#include "url.h"
#include "util/result.h"

namespace berth {

inline constexpr int kMaxRedirectHops = 5;
inline constexpr std::size_t kMaxBodyBytes = std::size_t{64} * 1024 * 1024;
inline constexpr std::size_t kMaxHeadBytes = std::size_t{64} * 1024;

// The analyzer's clang-analyzer-core.uninitialized.Assign false-positives on
// the implicit move through Result's std::variant; every field below has a
// default initializer, so there is nothing to initialize.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct HttpReply {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
  std::string body;
  int hops = 0;           // redirects followed to reach this reply
  std::string stop_note;  // why a redirect loop stopped before following

  // Exact lookup of an ALREADY-lowercased name; nullptr when absent. The
  // compare is deliberately case-sensitive so a caller that forgot to fold
  // case gets "not found" instead of a silently matched header.
  [[nodiscard]] const std::string* header(std::string_view name) const;
};

// Splits a wire response into head + parsed reply, returning the head length.
// Requires the full head ("\r\n\r\n") to be present. Does not touch the body.
[[nodiscard]] Result<std::pair<HttpReply, std::size_t>> parse_http_head(
    std::string_view wire);

enum class BodyMode {
  kContentLength,
  kChunked,
  kUntilClose,
};

// Selects the body framing from parsed headers (transfer-encoding wins).
[[nodiscard]] BodyMode body_mode_of(const HttpReply& reply);

// The declared Content-Length, or nullopt when absent, invalid, or declared
// more than once (a smuggling shape that is unusable rather than guessed at).
[[nodiscard]] std::optional<std::size_t> content_length_of(
    const HttpReply& reply);

// Decodes a chunked body. Returns nullopt while the input is incomplete;
// Err on a malformed chunk grammar or an oversized total.
[[nodiscard]] Result<std::optional<std::string>> decode_chunked(
    std::string_view body, std::size_t max_bytes);

// True when the response accumulated so far is complete under `mode`.
[[nodiscard]] bool response_is_complete(const std::string& wire,
                                        std::size_t head_len, BodyMode mode,
                                        std::size_t content_length);

enum class RedirectAction {
  kNotRedirect,      // not 3xx, or no redirect loop should run
  kFollow,           // target parsed, within cap, and allowlisted (§6.2)
  kStopCap,          // hop budget exhausted: return the redirect as-is
  kStopDenied,       // redirect target refused by the allowlist
  kStopBadLocation,  // missing or unresolvable Location
};

struct RedirectDecision {
  RedirectAction action = RedirectAction::kNotRedirect;
  std::string next_url;
};

// The hop budget a request actually gets: 0 unless the caller opted in with
// follow_allowlisted_redirects, and never above kMaxRedirectHops.
[[nodiscard]] int effective_redirect_budget(int max_redirect_hops,
                                            bool follow_allowlisted_redirects);

// The PURE redirect driver (§6.2): a target is followable only when
// hops_done is below the effective budget AND the target host passes the
// same allowlist gate as an initial request. Redirects are never followed
// blindly — this function is the only thing that can say kFollow.
[[nodiscard]] RedirectDecision next_redirect(const HttpReply& reply,
                                             const RequestKind kind,
                                             const std::string& current_url,
                                             int hops_done, int hop_budget);

}  // namespace berth
