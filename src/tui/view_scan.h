#pragma once

#include <string>
#include <vector>

#include "../analyzer/scan.h"
#include "../util/result.h"

namespace tui {

[[nodiscard]] std::vector<std::string> scan_prereq_lines(
    const berth::ScanReport& report);

// Never a process name — §7 forbids attributing foreign listeners.
[[nodiscard]] std::vector<std::string> scan_port_lines(
    const berth::ScanReport& report);

[[nodiscard]] std::vector<std::string> scan_service_lines(
    const berth::ScanReport& report);

[[nodiscard]] std::vector<std::string> scan_network_lines(
    const berth::ScanReport& report);

[[nodiscard]] std::string scan_summary_line(const berth::ScanReport& report);

[[nodiscard]] std::string scan_network_alert(const berth::ScanReport& report);

[[nodiscard]] std::vector<std::string> scan_idle_lines();

}
