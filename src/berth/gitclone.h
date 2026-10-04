#pragma once

// src/berth/gitclone.h — clone a PINNED commit with git and GIT_ASKPASS.
// The token never appears in argv, in the environment we pass, or in any
// log: it lives in a 0700 temp dir that is removed when the clone ends
// (dev.md §6.3). A token is bound to its host by EXACT comparison, never
// by substring, and HEAD is verified against the requested SHA so a moved
// branch can never sneak in (§6.6).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "../util/result.h"

namespace berth {

inline constexpr std::string_view kTokenHost = "github.com";

struct CloneRequest {
  std::string url;                 // https://github.com/o/r.git or file://...
  std::string commit;              // full 40-hex SHA
  std::filesystem::path dest_dir;  // created by this call; must be empty
  const std::string* token = nullptr;
  std::string token_host{kTokenHost};
  std::chrono::milliseconds timeout{600000};
  std::atomic<bool>* cancel = nullptr;
};

// Pure: may a token be attached to this URL? Exact host match only, https
// only. "https://github.com.evil.io/o/r" and "https://evil.io/?x=github.com"
// are refused.
[[nodiscard]] bool token_allowed_for_url(const std::string& url,
                                         std::string_view host);

[[nodiscard]] Result<void> clone_pinned(const CloneRequest& request);

// github.com convenience wrapper; validates owner/repo and SHA.
[[nodiscard]] Result<void> clone_pinned_github(
    const std::string& owner_repo, const std::string& commit,
    const std::filesystem::path& dest_dir, const std::string* token,
    std::chrono::milliseconds timeout, std::atomic<bool>* cancel);

}  // namespace berth
