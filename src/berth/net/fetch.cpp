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

// The client only needs how to reach an address, not whether it is v4 or v6.
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

}
Result<HttpReply> http_get(const std::string& url,
                           const HttpGetOptions& options, const Paths& paths) {
  return berth::http_get(url, options, paths,
                         ResolveHost(&resolve_to_addresses));
}

}
