#pragma once

// src/tui/view_scan.h — the System Scan view model (plan stage 6.5) as pure
// functions over berth::ScanReport. No FTXUI, no threads: the scan itself
// blocks (a bounded subprocess round trip, /proc reads and one HTTPS GET), so
// it runs on a worker, and everything drawn comes from here.
//
// One section per step of the scan, in the order the engine runs them, and
// every section says which of three things happened: it was not collected, it
// was collected and is empty, or it was collected and here is what it found.
// A section that renders as blank reads as a bug in the panel.
//
// The network section has its own emphasis because it is the only one whose
// failure the user can act on: the scan still COMPLETES offline, and says so.

#include <string>
#include <vector>

#include "../analyzer/scan.h"
#include "../util/result.h"

namespace tui {

// (a) which build tools the catalog needs are on PATH.
[[nodiscard]] std::vector<std::string> scan_prereq_lines(
    const berth::ScanReport& report);

// (b) the ports in use. Never a process name — §7: no attribution of foreign
// listeners, and the header of this file is the reason.
[[nodiscard]] std::vector<std::string> scan_port_lines(
    const berth::ScanReport& report);

// (c) the deployed services and their live state.
[[nodiscard]] std::vector<std::string> scan_service_lines(
    const berth::ScanReport& report);

// (d) the single allowlisted GET, its status and how long it took.
[[nodiscard]] std::vector<std::string> scan_network_lines(
    const berth::ScanReport& report);

// The headline: "everything needed is in place", or the first thing that is
// not. Shown above the sections so the answer is visible without scrolling.
[[nodiscard]] std::string scan_summary_line(const berth::ScanReport& report);

// The line the screen draws loud when the network check came back bad. Empty
// when the network answered, so the caller can skip the emphasis entirely.
[[nodiscard]] std::string scan_network_alert(const berth::ScanReport& report);

// Shown instead of the report when the scan has not run yet.
[[nodiscard]] std::vector<std::string> scan_idle_lines();

}  // namespace tui