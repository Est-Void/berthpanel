// Offline tests for egress.h (stage 3.6): the report renderer. The probe
// itself is the manual demo (`__egress-probe`, live network, §9).

#include "berth/net/egress.h"

#include <string>

#include "runner.h"

using berth::EgressProbeResult;
using berth::render_egress_report;

TEST(egress_report_renders_statuses_and_summary) {
  std::vector<EgressProbeResult> results;
  results.push_back(
      {.host = "github.com", .reachable = true, .detail = "status=301"});
  results.push_back(
      {.host = "api.github.com", .reachable = true, .detail = "status=403"});
  results.push_back({.host = "proxy.golang.org",
                     .reachable = false,
                     .detail = "http_get: timed out"});

  const std::string report = render_egress_report(results);
  CHECK(report.find("  ok   github.com") != std::string::npos);
  CHECK(report.find("status=403") != std::string::npos);
  CHECK(report.find("  FAIL proxy.golang.org") != std::string::npos);
  CHECK(report.find("2/3 endpoints reachable") != std::string::npos);
  CHECK(report.find("net.log") != std::string::npos);
}

TEST(egress_report_empty_table) {
  const std::string report = render_egress_report({});
  CHECK(report.find("0/0 endpoints reachable") != std::string::npos);
}
