// src/tui/view_scan.cpp — see view_scan.h for the contract.

#include "view_scan.h"

#include <string>
#include <utility>
#include <vector>

#include "../util/strutil.h"

namespace tui {

namespace {

std::string mark(const bool good) {
  return good ? "ok  " : "--  ";
}

// The "not collected" text a failed step shows. Loud on purpose: an unreadable
// section must not read as an empty one (§11).
std::string collection_failed(const std::string& what,
                              const std::string& error) {
  std::string line = "--  " + what + ": NOT COLLECTED";
  if (!error.empty()) {
    line += "\n      " + error;
  }
  return line;
}

std::string ms(const std::chrono::milliseconds elapsed) {
  return std::to_string(elapsed.count()) + "ms";
}

}  // namespace

std::vector<std::string> scan_prereq_lines(const berth::ScanReport& report) {
  std::vector<std::string> lines;
  if (!report.prereqs.done) {
    lines.push_back(collection_failed("build tools", report.prereqs.error));
    return lines;
  }
  std::size_t missing = 0;
  for (const berth::ToolStatus& tool : report.prereqs.tools) {
    if (!tool.present) {
      ++missing;
    }
    std::string line = mark(tool.present) + tool.name;
    if (tool.present) {
      line +=
          tool.version.empty() ? "  (version unknown)" : "  " + tool.version;
      line += "  " + tool.path;
    } else {
      line += "  not on PATH — a recipe that needs it cannot be built";
    }
    lines.push_back(std::move(line));
  }
  lines.push_back("      " +
                  std::to_string(report.prereqs.tools.size() - missing) + "/" +
                  std::to_string(report.prereqs.tools.size()) +
                  " tools the catalog needs are present");
  return lines;
}

std::vector<std::string> scan_port_lines(const berth::ScanReport& report) {
  std::vector<std::string> lines;
  if (!report.ports.done) {
    lines.push_back(collection_failed("ports", report.ports.error));
    return lines;
  }
  const berth::PortReport& ports = report.ports.report;
  std::size_t ours = 0;
  std::size_t wildcard = 0;
  for (const berth::ListeningPort& port : ports.ports) {
    if (port.owned) {
      ++ours;
    }
    if (port.wildcard) {
      ++wildcard;
    }
    std::string line = mark(port.owned || port.loopback) + port.address + ":" +
                       std::to_string(port.port);
    if (port.owned) {
      line += "  yours (a berthpanel service)";
    } else if (port.wildcard) {
      line += "  on every interface";
    } else if (port.loopback) {
      line += "  loopback only";
    } else {
      line += "  reachable from this network";
    }
    lines.push_back(std::move(line));
  }
  if (ports.ports.empty()) {
    lines.push_back("      nothing is listening");
  }
  lines.push_back("      " + std::to_string(ports.ports.size()) +
                  " listening ports, " + std::to_string(ours) +
                  " of them yours");
  if (wildcard > 0) {
    lines.push_back("      " + std::to_string(wildcard) +
                    " bound on every interface");
  }
  if (!ports.ipv6_read) {
    lines.push_back(
        "      no /proc/net/tcp6 on this host" +
        (ports.ipv6_error.empty() ? std::string() : ": " + ports.ipv6_error));
  }
  if (ports.unparsed > 0) {
    // A shortened list reads as "nothing is listening", so say what was
    // skipped.
    lines.push_back("      " + std::to_string(ports.unparsed) +
                    " unreadable lines were skipped");
  }
  lines.push_back(
      "      ports are NOT attributed to processes: this report is "
      "for you, not for watching the machine");
  return lines;
}

std::vector<std::string> scan_service_lines(const berth::ScanReport& report) {
  std::vector<std::string> lines;
  if (!report.services.done) {
    lines.push_back(collection_failed("services", report.services.error));
    return lines;
  }
  for (const berth::ServiceRow& row : report.services.rows) {
    const std::string state(row.state == berth::ServiceState::kActive ? "active"
                            : row.state == berth::ServiceState::kInactive
                                ? "inactive"
                                : "unknown");
    std::string line = mark(row.state == berth::ServiceState::kActive) +
                       row.service_name + "  " + state;
    if (row.port != 0) {
      line += "  port " + std::to_string(row.port);
    }
    if (!row.commit_short.empty()) {
      line += "  " + row.commit_short;
    }
    lines.push_back(std::move(line));
  }
  if (report.services.rows.empty()) {
    lines.push_back("      no services registered — nothing is deployed yet");
  }
  return lines;
}

std::vector<std::string> scan_network_lines(const berth::ScanReport& report) {
  std::vector<std::string> lines;
  if (!report.network.done) {
    lines.push_back(collection_failed("network", report.network.error));
    return lines;
  }
  if (report.network.error.empty() && report.network.ok) {
    lines.push_back("ok    github.com answered " + report.network.detail +
                    " in " + ms(report.network.elapsed));
    return lines;
  }
  lines.push_back("--    github.com did not answer in " +
                  ms(report.network.elapsed));
  if (!report.network.error.empty()) {
    lines.push_back("      " + report.network.error);
  } else if (!report.network.detail.empty()) {
    lines.push_back("      " + report.network.detail);
  }
  lines.push_back(
      "      one GET, one attempt, 2s. the rest of this scan is "
      "local and is still valid");
  return lines;
}

std::string scan_summary_line(const berth::ScanReport& report) {
  if (report.cancelled) {
    return "scan cancelled — the sections below are what was collected";
  }
  // "Could not be read" outranks "something is missing": a report built from
  // two sections and a hole is not a green light.
  std::vector<std::string> uncollected;
  if (!report.prereqs.done) {
    uncollected.emplace_back("the build tools");
  }
  if (!report.ports.done) {
    uncollected.emplace_back("the ports");
  }
  if (!report.services.done) {
    uncollected.emplace_back("your services");
  }
  if (!uncollected.empty()) {
    return "scan incomplete — not collected: " + berth::join(uncollected, ", ");
  }
  std::size_t missing = 0;
  for (const berth::ToolStatus& tool : report.prereqs.tools) {
    if (!tool.present) {
      ++missing;
    }
  }
  if (missing > 0) {
    return "ready, with gaps: " + std::to_string(missing) + " of " +
           std::to_string(report.prereqs.tools.size()) +
           " build tools are missing";
  }
  if (report.network.done && !report.network.ok) {
    return "ready to deploy; the network check did not answer";
  }
  return "ready to deploy";
}

std::string scan_network_alert(const berth::ScanReport& report) {
  // `error` is only set when the probe itself failed; a refused connection
  // arrives as ok=false with the reason in `detail`, so gating on `error` here
  // would make the alert unreachable in exactly the case it exists for.
  if (!report.network.done || report.network.ok) {
    return {};
  }
  const std::string& why = report.network.error.empty() ? report.network.detail
                                                        : report.network.error;
  return "the network check FAILED: " + why +
         "\n  a deploy needs github.com; everything else on this screen is "
         "local and still true";
}

std::vector<std::string> scan_idle_lines() {
  return {
      "no scan has run yet.",
      "",
      "System Scan reports four things and nothing else:",
      "  * the build tools the catalog needs, on PATH",
      "  * the ports in use — never attributed to processes (§7)",
      "  * the state of the services you deployed",
      "  * one bounded HTTPS GET to github.com",
      "",
      "Press r to run it.",
  };
}

}  // namespace tui