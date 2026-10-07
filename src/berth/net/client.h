#pragma once

// src/berth/net/client.h — the allowlisted HTTPS exchange (dev.md §6.1–6.3).
//
// Invariants owned here:
//   §6.1  every attempt is net.logged, allowed or refused, and the refusal
//         is logged BEFORE the refusal is returned.
//   §6.2  the allowlist gate runs before EVERY socket connect — including
//         each redirect hop. Redirects are never auto-followed;
//         protocol.h's next_redirect decides, bounded by kMaxRedirectHops.
//   §6.3  a bearer token goes out ONLY when the request host is exactly
//         "api.github.com". Never in logs, never in argv.
//   §11   a complete HTTP response is a success at ANY status: 403 is not a
//         network outage. Err is transport-level failure only.
//
// The resolver is INJECTED (see ResolveHost) rather than called directly:
// that is what breaks the httpclient↔resolver dependency cycle, and it keeps
// this layer testable without any DNS. fetch.h wires the real one.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "../../core/allowlist.h"
#include "../../core/paths.h"
#include "protocol.h"
#include "url.h"
#include "util/result.h"

namespace berth {

// Host → addresses, as text. Called only when HttpGetOptions::pinned_ip is
// null, and only AFTER the allowlist gate has passed: a non-allowlisted host
// is never resolved, let alone connected to.
using ResolveHost = std::function<Result<std::vector<std::string>>(
    std::string_view host, std::chrono::milliseconds budget,
    const std::atomic<bool>* cancel, const Paths& paths)>;

struct HttpGetOptions {
  RequestKind kind = RequestKind::kApi;
  std::string purpose = "https-get";  // net.log purpose field
  int max_redirect_hops = 0;          // 0 = never follow; capped by
                                      // kMaxRedirectHops (excess = Err)
  // Explicit opt-in for callers that KNOW their upstream redirects (GitHub
  // release assets 302 to release-assets.githubusercontent.com, which is on
  // the allowlist for exactly that reason). Every hop is still gated per
  // hop; a redirect to a non-allowlisted host is refused.
  bool follow_allowlisted_redirects = false;
  std::chrono::milliseconds per_attempt_timeout{30000};
  int max_attempts = 3;  // transport failures only
  std::size_t max_body_bytes = kMaxBodyBytes;
  const std::string* bearer_token = nullptr;  // §6.3: api.github.com only
  const std::string* pinned_ip = nullptr;  // skip the resolver (DoH bootstrap)
  // Polled, never written: the exit-signalling handler owns the flag.
  const std::atomic<bool>* cancel = nullptr;
};

// One logical GET: per-hop allowlist gating, resolution through `resolve`,
// TLS, bounded retries on transport failure, and the non-auto redirect loop.
// A request that needs resolution but was given no resolver is an Err, not a
// silent skip.
[[nodiscard]] Result<HttpReply> http_get(const std::string& url,
                                         const HttpGetOptions& options,
                                         const Paths& paths,
                                         const ResolveHost& resolve);

}  // namespace berth
