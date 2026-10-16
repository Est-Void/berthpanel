#pragma once

// The token never enters argv, env or logs: GIT_ASKPASS in a 0700 temp dir
// that dies with the clone (§6.3). Its host match is EXACT; HEAD is verified.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "../util/result.h"

namespace berth {

inline constexpr std::string_view kTokenHost = "github.com";

struct CloneRequest {
  std::string url;  // https://github.com/o/r.git or file://...
  std::string commit;
  std::filesystem::path dest_dir;
  const std::string* token = nullptr;
  std::string token_host{kTokenHost};
  std::chrono::milliseconds timeout{600000};
  std::atomic<bool>* cancel = nullptr;
};

// May a token be attached to this URL? Exact host match, https only.
[[nodiscard]] bool token_allowed_for_url(const std::string& url,
                                         std::string_view host);

[[nodiscard]] Result<void> clone_pinned(const CloneRequest& request);

[[nodiscard]] Result<void> clone_pinned_github(
    const std::string& owner_repo, const std::string& commit,
    const std::filesystem::path& dest_dir, const std::string* token,
    std::chrono::milliseconds timeout, std::atomic<bool>* cancel);

// Annotated tags are peeled (^{} wins). No token: the draft flow is public-repo
// only; tests may pass file://.
[[nodiscard]] Result<std::string> resolve_ref_sha(
    const std::string& url, const std::string& ref,
    std::chrono::milliseconds timeout, std::atomic<bool>* cancel);

// Front-door policy for BYO egress (§6.1): https and host exactly github.com.
[[nodiscard]] Result<std::string> github_owner_repo_from_url(
    const std::string& url);

}
