#pragma once

// Deliberately dumb: no pid attribution, that is surveillance (§7); every step
// bounded; the one network check is an allowlisted HTTPS GET, never ICMP/DNS.

#include <atomic>
#include <chrono>
#include <string>

#include "../berth/service_ops.h"
#include "../berth/toolcheck.h"
#include "../core/paths.h"
#include "../util/result.h"
#include "ports.h"

namespace berth {

// Every step is optional: a section that could not be collected says so.
struct ScanReport {
  struct Prerequisites {
    bool done = false;
    std::vector<ToolStatus> tools;  // present/absent, never a failure
    std::string error;
  };
  struct Ports {
    bool done = false;
    PortReport report;
    std::string error;
  };
  struct Services {
    bool done = false;
    std::vector<ServiceRow> rows;
    std::string error;
  };
  struct Network {
    bool done = false;
    bool ok = false;
    std::string detail;
    std::chrono::milliseconds elapsed{0};
    std::string error;
  };

  Prerequisites prereqs;
  Ports ports;
  Services services;
  Network network;
  // The screen says "cancelled", never "everything is fine" (§11).
  bool cancelled = false;
};

// The single GET: the only place that may probe "is the network up".
[[nodiscard]] Result<bool> probe_network_once(const Paths& paths,
                                              std::atomic<bool>* cancel,
                                              std::chrono::milliseconds timeout,
                                              std::string* detail);

// One worker, nothing unbounded; proc_net_root exists for the tests.
[[nodiscard]] Result<ScanReport> run_system_scan(
    const Paths& paths, std::atomic<bool>* cancel,
    const std::filesystem::path& proc_net_root = "/proc/net");

}
