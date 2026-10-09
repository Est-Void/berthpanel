// src/berth/net/fetch.cpp — see fetch.h for the contract.

#include "fetch.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "doh.h"
#include "resolver.h"

namespace berth {

namespace {

// Adapts resolve_host's ResolvedIp to the plain address list the client
// asks for. Deliberately thin: the client does not need to know whether an
// address is v4 or v6, only how to reach it.
[[nodiscard]] Result<std::vector<std::string>> resolve_to_addresses(
    const std::string_view host, const std::chrono::milliseconds budget,
    const std::atomic<bool>* cancel, const Paths& paths) {
  Result<std::vector<ResolvedIp>> resolved =
      resolve_host(std::string(host), budget, paths, cancel, doh_fallback());
  if (resolved.is_err())
    return std::move(resolved).error();

  std::vector<std::string> addresses;
  addresses.reserve(resolved.value().size());
  for (const ResolvedIp& ip : resolved.value()) {
    addresses.push_back(ip.ip);
  }
  return addresses;
}

}  // namespace

Result<HttpReply> http_get(const std::string& url,
                           const HttpGetOptions& options, const Paths& paths) {
  return berth::http_get(url, options, paths,
                         ResolveHost(&resolve_to_addresses));
}

}  // namespace berth
