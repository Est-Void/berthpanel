#pragma once

// Pure: bytes in, parsed values out — no socket, no policy but the allowlist.

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

// clang-analyzer false-positives on the implicit move through Result.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct HttpReply {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
  std::string body;
  int hops = 0;
  std::string stop_note;  // why a redirect loop stopped before following

  // Case-sensitive on purpose: a caller that forgot to fold gets "not found".
  [[nodiscard]] const std::string* header(std::string_view name) const;
};

// Requires the full head ("\r\n\r\n"); does not touch the body.
[[nodiscard]] Result<std::pair<HttpReply, std::size_t>> parse_http_head(
    std::string_view wire);

enum class BodyMode {
  kContentLength,
  kChunked,
  kUntilClose,
};

// transfer-encoding wins over content-length.
[[nodiscard]] BodyMode body_mode_of(const HttpReply& reply);

// nullopt when absent, invalid, or declared twice (a smuggling shape).
[[nodiscard]] std::optional<std::size_t> content_length_of(
    const HttpReply& reply);

// nullopt while incomplete; Err on bad chunk grammar or an oversized total.
[[nodiscard]] Result<std::optional<std::string>> decode_chunked(
    std::string_view body, std::size_t max_bytes);

[[nodiscard]] bool response_is_complete(const std::string& wire,
                                        std::size_t head_len, BodyMode mode,
                                        std::size_t content_length);

enum class RedirectAction {
  kNotRedirect,
  kFollow,  // target parsed, within cap, and allowlisted (§6.2)
  kStopCap,
  kStopDenied,
  kStopBadLocation,
};

struct RedirectDecision {
  RedirectAction action = RedirectAction::kNotRedirect;
  std::string next_url;
};

// 0 unless the caller opted in, never above kMaxRedirectHops.
[[nodiscard]] int effective_redirect_budget(int max_redirect_hops,
                                            bool follow_allowlisted_redirects);

// §6.2: kFollow only inside the budget AND past the same allowlist gate.
[[nodiscard]] RedirectDecision next_redirect(const HttpReply& reply,
                                             const RequestKind kind,
                                             const std::string& current_url,
                                             int hops_done, int hop_budget);

}
