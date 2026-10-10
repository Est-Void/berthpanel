#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "../../core/paths.h"
#include "../../util/result.h"

namespace berth {

struct EgressProbeResult {
  std::string host;
  bool reachable = false;
  std::string detail;  // "status=200", "status=403"
};

[[nodiscard]] Result<std::vector<EgressProbeResult>> run_egress_probe(
    const Paths& paths, std::atomic<bool>* cancel,
    std::chrono::milliseconds per_host_timeout);

// Pure rendering of the probe table (unit-tested offline).
[[nodiscard]] std::string render_egress_report(
    const std::vector<EgressProbeResult>& results);

}  // namespace berth
