// src/berth/egress.cpp — see egress.h for the contract.

#include "egress.h"

#include <string>

#include "httpclient.h"
#include "netlog.h"

namespace berth {

Result<std::vector<EgressProbeResult>> run_egress_probe(
    const Paths& paths, std::atomic<bool>* cancel,
    const std::chrono::milliseconds per_host_timeout) {
  std::vector<EgressProbeResult> results;
  for (const std::string_view host : allowed_endpoints()) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      break;
    }
    HttpGetOptions options;
    options.kind = RequestKind::kApi;
    options.purpose = "egress-probe";
    options.max_redirect_hops = 0;
    options.per_attempt_timeout = per_host_timeout;
    options.max_attempts = 1;
    options.cancel = cancel;

    const std::string url = std::string("https://") + std::string(host) + "/";
    Result<HttpReply> reply = http_get(url, options, paths);
    EgressProbeResult result;
    result.host = std::string(host);
    if (reply.is_ok()) {
      result.reachable = true;
      result.detail = "status=" + std::to_string(reply.value().status);
    } else {
      result.reachable = false;
      result.detail = reply.error().ctx();
    }
    results.push_back(std::move(result));
  }
  return results;
}

std::string render_egress_report(const std::vector<EgressProbeResult>& results) {
  std::string out;
  std::size_t reachable = 0;
  std::size_t host_width = 0;
  for (const EgressProbeResult& r : results) {
    host_width = std::max(host_width, r.host.size());
    if (r.reachable) ++reachable;
  }
  for (const EgressProbeResult& r : results) {
    out += r.reachable ? "  ok   " : "  FAIL ";
    out += r.host;
    out += std::string(host_width - r.host.size() + 2, ' ');
    out += r.detail;
    out += "\n";
  }
  out += "\n  " + std::to_string(reachable) + "/" +
         std::to_string(results.size()) + " endpoints reachable\n";
  out += "  every attempt was appended to net.log\n";
  return out;
}

}  // namespace berth
