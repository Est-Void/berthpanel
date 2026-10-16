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
  std::string detail;
};

[[nodiscard]] Result<std::vector<EgressProbeResult>> run_egress_probe(
    const Paths& paths, std::atomic<bool>* cancel,
    std::chrono::milliseconds per_host_timeout);

[[nodiscard]] std::string render_egress_report(
    const std::vector<EgressProbeResult>& results);

}
