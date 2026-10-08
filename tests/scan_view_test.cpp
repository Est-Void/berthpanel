// Tests for the System Scan view model (stage 6.5). The scan engine itself
// needs a live network and a live systemd, so what is tested HERE is the
// promise the screen makes about a report: every section either says what it
// found or says it could not be collected — never both blank (§11).

#include "tui/view_scan.h"

#include <string>
#include <vector>

#include "runner.h"

namespace {

bool contains(const std::vector<std::string>& haystack,
              const std::string_view needle) {
  for (const std::string& line : haystack) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

berth::ToolStatus tool(const std::string& name, const bool present) {
  berth::ToolStatus status;
  status.name = name;
  status.present = present;
  status.path = present ? "/usr/bin/" + name : "";
  status.version = present ? "1.2.3" : "";
  return status;
}

berth::ScanReport collected_report() {
  berth::ScanReport report;
  report.prereqs.done = true;
  report.prereqs.tools = {tool("go", true), tool("bun", false)};

  report.ports.done = true;
  report.ports.report.ipv6_read = true;
  berth::ListeningPort ours;
  ours.address = "127.0.0.1";
  ours.port = 8080;
  ours.loopback = true;
  ours.owned = true;
  berth::ListeningPort theirs;
  theirs.address = "0.0.0.0";
  theirs.port = 1716;
  theirs.wildcard = true;
  report.ports.report.ports = {ours, theirs};

  report.services.done = true;
  berth::ServiceRow row;
  row.service_name = "berthpanel-memos";
  row.state = berth::ServiceState::kActive;
  row.port = 8080;
  row.commit_short = "2b2192d";
  report.services.rows = {row};

  report.network.done = true;
  report.network.ok = true;
  report.network.detail = "status=200";
  report.network.elapsed = std::chrono::milliseconds(180);
  return report;
}

}  // namespace

TEST(scan_summary_reports_the_first_thing_that_is_not_ready) {
  const berth::ScanReport report = collected_report();
  // One tool is missing, so "ready to deploy" would be a lie.
  CHECK(tui::scan_summary_line(report).find("with gaps") != std::string::npos);
  CHECK(tui::scan_summary_line(report).find("1 of 2") != std::string::npos);

  berth::ScanReport ready = collected_report();
  ready.prereqs.tools = {tool("go", true), tool("bun", true)};
  CHECK(tui::scan_summary_line(ready) == "ready to deploy");
}

TEST(scan_summary_says_incomplete_rather_than_ready) {
  berth::ScanReport no_tools = collected_report();
  no_tools.prereqs.done = false;
  no_tools.prereqs.error = "PATH is empty";
  CHECK(tui::scan_summary_line(no_tools).find("incomplete") !=
        std::string::npos);

  berth::ScanReport no_ports = collected_report();
  no_ports.ports.done = false;
  CHECK(tui::scan_summary_line(no_ports).find("incomplete") !=
        std::string::npos);

  berth::ScanReport cancelled = collected_report();
  cancelled.cancelled = true;
  CHECK(tui::scan_summary_line(cancelled).find("cancelled") !=
        std::string::npos);
}

TEST(scan_prereqs_name_every_tool_and_the_missing_one) {
  const std::vector<std::string> lines =
      tui::scan_prereq_lines(collected_report());
  CHECK(contains(lines, "go"));
  CHECK(contains(lines, "1.2.3"));
  CHECK(contains(lines, "bun"));
  CHECK(contains(lines, "not on PATH"));
  CHECK(contains(lines, "1/2 tools the catalog needs are present"));
}

TEST(scan_ports_never_attribute_a_listener_to_a_process) {
  // §7: the report is useless to anyone but the user BY DESIGN. The only
  // listener-related claim it may make is about ports in our own registry.
  const std::vector<std::string> lines =
      tui::scan_port_lines(collected_report());
  CHECK(contains(lines, "127.0.0.1:8080"));
  CHECK(contains(lines, "yours (a berthpanel service)"));
  CHECK(contains(lines, "0.0.0.0:1716"));
  CHECK(contains(lines, "on every interface"));
  CHECK(contains(lines, "2 listening ports, 1 of them yours"));
  CHECK(contains(lines, "NOT attributed to processes"));
  // Nothing that looks like a pid, a command or a unit of somebody else's.
  for (const std::string& line : lines) {
    CHECK(line.find("pid") == std::string::npos);
    CHECK(line.find("comm") == std::string::npos);
  }
}

TEST(scan_ports_say_when_ipv6_was_absent_and_when_rows_were_skipped) {
  berth::ScanReport report = collected_report();
  report.ports.report.ipv6_read = false;
  report.ports.report.ipv6_error = "No such file or directory";
  report.ports.report.unparsed = 3;
  const std::vector<std::string> lines = tui::scan_port_lines(report);
  CHECK(contains(lines, "no /proc/net/tcp6 on this host"));
  // A silently shortened list reads as "nothing is listening".
  CHECK(contains(lines, "3 unreadable lines were skipped"));
}

TEST(scan_ports_say_when_nothing_is_listening) {
  berth::ScanReport report = collected_report();
  report.ports.report.ports.clear();
  const std::vector<std::string> lines = tui::scan_port_lines(report);
  CHECK(contains(lines, "nothing is listening"));
  CHECK(contains(lines, "0 listening ports, 0 of them yours"));
}

TEST(scan_services_name_the_unit_the_state_and_the_port) {
  const std::vector<std::string> lines =
      tui::scan_service_lines(collected_report());
  CHECK(contains(lines, "berthpanel-memos"));
  CHECK(contains(lines, "active"));
  CHECK(contains(lines, "port 8080"));
  CHECK(contains(lines, "2b2192d"));

  berth::ScanReport empty = collected_report();
  empty.services.rows.clear();
  CHECK(contains(tui::scan_service_lines(empty), "nothing is deployed yet"));
}

TEST(scan_network_reports_ok_with_its_status_and_its_time) {
  const std::vector<std::string> lines =
      tui::scan_network_lines(collected_report());
  CHECK(contains(lines, "github.com answered status=200 in 180ms"));
  // A probe that retries is a screen that takes twice as long to say "offline".
  CHECK(!contains(lines, "retry"));
  CHECK(tui::scan_network_alert(collected_report()).empty());
}

TEST(scan_network_failure_is_its_own_alert_and_does_not_stop_the_scan) {
  berth::ScanReport report = collected_report();
  report.prereqs.tools = {tool("go", true)};  // nothing missing locally
  report.network.ok = false;
  report.network.detail = "connect: network is unreachable";
  report.network.elapsed = std::chrono::milliseconds(2010);

  const std::vector<std::string> lines = tui::scan_network_lines(report);
  CHECK(contains(lines, "did not answer in 2010ms"));
  CHECK(contains(lines, "one GET, one attempt, 2s"));

  const std::string alert = tui::scan_network_alert(report);
  CHECK(alert.find("FAILED") != std::string::npos);
  CHECK(alert.find("everything else on this screen is local") !=
        std::string::npos);
  // The other sections are still there and still valid: the scan COMPLETES
  // offline (§7).
  CHECK(tui::scan_summary_line(report).find("did not answer") !=
        std::string::npos);
  CHECK(contains(tui::scan_prereq_lines(report), "go"));
}

TEST(scan_a_section_that_could_not_be_collected_says_so_loudly) {
  // The three ways a section can be absent, and none of them may render as an
  // empty section.
  berth::ScanReport report = collected_report();
  report.prereqs.done = false;
  report.prereqs.error = "PATH is empty";
  report.ports.done = false;
  report.ports.error = "reading /proc/net/tcp failed: permission denied";
  report.services.done = false;
  report.services.error = "systemctl is-active: exit 4";
  report.network.done = false;
  report.network.error = "cancelled";

  CHECK(contains(tui::scan_prereq_lines(report), "NOT COLLECTED"));
  CHECK(contains(tui::scan_prereq_lines(report), "PATH is empty"));
  CHECK(contains(tui::scan_port_lines(report), "NOT COLLECTED"));
  CHECK(contains(tui::scan_port_lines(report), "/proc/net/tcp"));
  CHECK(contains(tui::scan_service_lines(report), "NOT COLLECTED"));
  CHECK(contains(tui::scan_service_lines(report), "systemctl is-active"));
  CHECK(contains(tui::scan_network_lines(report), "NOT COLLECTED"));
}

TEST(scan_idle_text_says_what_the_scan_does_and_how_to_run_it) {
  const std::vector<std::string> lines = tui::scan_idle_lines();
  CHECK(contains(lines, "no scan has run yet"));
  CHECK(contains(lines, "never attributed to processes"));
  CHECK(contains(lines, "one bounded HTTPS GET to github.com"));
  CHECK(contains(lines, "Press r to run it"));
}