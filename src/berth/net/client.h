#pragma once

// §6.1 every attempt is net.logged, the refusal logged before it is returned;
// §6.2 the gate runs before every connect, every redirect hop included, and
// redirects are never auto-followed; §6.3 a bearer token goes out only for an
// exact "api.github.com", never into logs or argv. §11: any complete HTTP
// response is a success, 403 included.

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

// Only called after the gate passed: a non-allowlisted host is never resolved.
using ResolveHost = std::function<Result<std::vector<std::string>>(
    std::string_view host, std::chrono::milliseconds budget,
    const std::atomic<bool>* cancel, const Paths& paths)>;

struct HttpGetOptions {
  RequestKind kind = RequestKind::kApi;
  std::string purpose = "https-get";
  int max_redirect_hops = 0;  // 0 = never follow; excess over the cap is an Err
  // Opt-in for callers that know their upstream redirects (GitHub release
  // assets); every hop is still gated.
  bool follow_allowlisted_redirects = false;
  std::chrono::milliseconds per_attempt_timeout{30000};
  int max_attempts = 3;  // transport failures only
  std::size_t max_body_bytes = kMaxBodyBytes;
  const std::string* bearer_token = nullptr;  // §6.3: api.github.com only
  const std::string* pinned_ip = nullptr;
  // Polled, never written: the exit-signalling handler owns the flag.
  const std::atomic<bool>* cancel = nullptr;
};

// A request that needs resolution but was given no resolver is an Err.
[[nodiscard]] Result<HttpReply> http_get(const std::string& url,
                                         const HttpGetOptions& options,
                                         const Paths& paths,
                                         const ResolveHost& resolve);

}
