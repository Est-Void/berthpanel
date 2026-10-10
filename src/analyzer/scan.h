#pragma once

// src/analyzer/scan.h — System Scan: prerequisites, listening ports, the live
// state of the deployed services, and ONE bounded network check (dev.md §7).
//
// Three rules this module exists to hold:
//
//   * it stays DELIBERATELY DUMB. No attribution of listening ports to foreign
//     processes — that is surveillance, it needs a justification, and the old
//     README claimed a capability the code refused to build (§7);
//   * every step is BOUNDED. A best-effort UI probe must be time-bounded or
//     the screen never completes offline (§7, §11: an unbounded retry in a
//     probe is the frozen-screen bug all over again);
//   * the network check is a single allowlisted, net.log-recorded HTTPS GET to
//     github.com with max_attempts=1. Never ICMP, never raw DNS.

#include <atomic>
#include <chrono>
#include <string>

#include "../berth/service_ops.h"
#include "../berth/toolcheck.h"
#include "../core/paths.h"
#include "../util/result.h"
#include "ports.h"

namespace berth {

// Every step is separate and optional: one failing step must not blank the
// report. A section that could not be collected says so in its own text.
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
    std::vector<ServiceRow> rows;  // registry + live is-active
    std::string error;
  };
  struct Network {
    bool done = false;
    bool ok = false;
    std::string detail;  // "status=200" or the failure text
    std::chrono::milliseconds elapsed{0};
    std::string error;
  };

  Prerequisites prereqs;
  Ports ports;
  Services services;
  Network network;
  // True when the scan stopped early because the caller cancelled (§11). The
  // screen says "cancelled" rather than "everything is fine".
  bool cancelled = false;
};

// The single GET System Scan makes. Exposed so the CLI and the tests can ask
// the same question with the same bounds; nothing else in the codebase should
// probe "is the network up".
[[nodiscard]] Result<bool> probe_network_once(const Paths& paths,
                                              std::atomic<bool>* cancel,
                                              std::chrono::milliseconds timeout,
                                              std::string* detail);

// The whole scan, one worker, no unbounded anything. `proc_net_root` exists
// for the tests; production leaves it at /proc/net.
[[nodiscard]] Result<ScanReport> run_system_scan(
    const Paths& paths, std::atomic<bool>* cancel,
    const std::filesystem::path& proc_net_root = "/proc/net");

}  // namespace berth