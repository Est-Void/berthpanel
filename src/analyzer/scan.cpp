#include "scan.h"

#include <string>
#include <utility>

#include "../berth/net/fetch.h"

namespace berth {

namespace {

// On the allowlist, and the least surprising reachability question there is.
constexpr std::string_view kProbeUrl = "https://github.com/";
constexpr std::string_view kProbePurpose = "system-scan";

bool cancelled(const std::atomic<bool>* cancel) {
  return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

}
Result<bool> probe_network_once(const Paths& paths, std::atomic<bool>* cancel,
                                const std::chrono::milliseconds timeout,
                                std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  HttpGetOptions options;
  options.kind = RequestKind::kApi;
  options.purpose = std::string(kProbePurpose);
  // A probe, not a fetch: no redirect chasing, no retry (§11 frozen screen).
  options.max_redirect_hops = 0;
  options.per_attempt_timeout = timeout;
  options.max_attempts = 1;
  options.cancel = cancel;

  const Result<HttpReply> reply =
      http_get(std::string(kProbeUrl), options, paths);
  if (reply.is_err()) {
    if (detail != nullptr) {
      *detail = reply.error().ctx();
    }
    return false;
  }
  if (detail != nullptr) {
    // 403 is a reachable network with a rate limit, not an outage (§11).
    *detail = "status=" + std::to_string(reply.value().status);
  }
  return true;
}

Result<ScanReport> run_system_scan(const Paths& paths,
                                   std::atomic<bool>* cancel,
                                   const std::filesystem::path& proc_net_root) {
  ScanReport report;

  Result<std::vector<ToolStatus>> tools = check_tools(catalog_tools(), cancel);
  if (tools.is_err()) {
    report.prereqs.error = tools.error().ctx();
  } else {
    report.prereqs.done = true;
    report.prereqs.tools = std::move(tools).value();
  }
  if (cancelled(cancel)) {
    report.cancelled = true;
    return report;
  }

  // The /proc/net read happens here, never on the render thread.
  Result<PortReport> ports = listening_ports(paths, proc_net_root);
  if (ports.is_err()) {
    report.ports.error = ports.error().ctx();
  } else {
    report.ports.done = true;
    report.ports.report = std::move(ports).value();
  }

  Result<std::vector<ServiceRow>> rows = service_rows(paths, cancel);
  if (rows.is_err()) {
    report.services.error = rows.error().ctx();
  } else {
    report.services.done = true;
    report.services.rows = std::move(rows).value();
  }
  if (cancelled(cancel)) {
    report.cancelled = true;
    return report;
  }

  // Runs LAST: the only step that can wait on something outside this machine.
  const auto started = std::chrono::steady_clock::now();
  std::string detail;
  const Result<bool> reachable = probe_network_once(
      paths, cancel, std::chrono::milliseconds(2000), &detail);
  report.network.done = true;
  report.network.elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started);
  if (reachable.is_err()) {
    report.network.error = reachable.error().ctx();
  } else {
    report.network.ok = reachable.value();
    report.network.detail = detail;
  }
  if (cancelled(cancel)) {
    report.cancelled = true;
  }
  return report;
}

}
