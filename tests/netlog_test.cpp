// Tests for netlog.h (stage 3.1). The first test PINS the allowlist
// exactly: dev.md §6.1 — kAllowedEndpoints and this test change in the
// SAME commit, with a public justification in the commit message. If you
// touched the table, this test failing is the procedure working.

#include "netlog.h"

#include <algorithm>
#include <filesystem>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"

using berth::EndpointClass;
using berth::RequestKind;

TEST(netlog_allowlist_is_exactly_the_documented_set) {
  const std::vector<std::string_view> table = berth::allowed_endpoints();
  const std::vector<std::string_view> kDocumented = {
      "api.github.com",
      "cdn.jsdelivr.net",
      "cloudflare-dns.com",
      "codeload.github.com",
      "dl.google.com",
      "github.com",
      "proxy.golang.org",
      "raw.githubusercontent.com",
      "release-assets.githubusercontent.com",
      "storage.googleapis.com",
  };
  CHECK(table.size() == kDocumented.size());
  for (std::size_t i = 0; i < kDocumented.size(); ++i) {
    CHECK(table[i] == kDocumented[i]);
  }
}

TEST(netlog_allowlisted_hosts_pass_exact_match) {
  for (const std::string_view host : berth::allowed_endpoints()) {
    CHECK(berth::classify_endpoint(host) == EndpointClass::kAllowed);
  }
  // Case-insensitive exact match.
  CHECK(berth::classify_endpoint("API.GITHUB.COM") == EndpointClass::kAllowed);
  CHECK(berth::classify_endpoint("GitHub.Com") == EndpointClass::kAllowed);
}

TEST(netlog_near_misses_are_denied) {
  // Substring, suffix and prefix attacks on allowlisted names (§6.3:
  // exact match, never substring).
  for (const std::string_view hostile : {
           "xapi.github.com",
           "api.github.com.evil.io",
           "evil-api.github.com",
           "github.com.evil.io",
           "github.com2",
           "raw.githubusercontent.com.attacker.test",
           "api.github.com:8080.evil.io",
           "api.githu",
           "",
       }) {
    CHECK(berth::classify_endpoint(hostile) == EndpointClass::kDenied);
  }
}

TEST(netlog_loopback_classified_separately) {
  for (const std::string_view host :
       {"127.0.0.1", "::1", "[::1]", "localhost", "LOCALHOST"}) {
    CHECK(berth::classify_endpoint(host) == EndpointClass::kLoopbackOnly);
  }
}

TEST(netlog_loopback_permitted_only_for_health_checks) {
  CHECK(berth::endpoint_permitted(EndpointClass::kAllowed, RequestKind::kApi));
  CHECK(berth::endpoint_permitted(EndpointClass::kAllowed,
                                  RequestKind::kHealthCheck));
  CHECK(berth::endpoint_permitted(EndpointClass::kLoopbackOnly,
                                  RequestKind::kHealthCheck));
  CHECK(!berth::endpoint_permitted(EndpointClass::kLoopbackOnly,
                                   RequestKind::kApi));
  CHECK(!berth::endpoint_permitted(EndpointClass::kDenied, RequestKind::kApi));
  CHECK(!berth::endpoint_permitted(EndpointClass::kDenied,
                                   RequestKind::kHealthCheck));
}

TEST(netlog_appends_lines_and_creates_state_dir) {
  const TempDir tmp;
  berth::Paths paths;
  paths.state_dir = tmp.path() / "state" / "berthpanel";
  paths.net_log = paths.state_dir / "net.log";  // dir does not exist yet

  const auto first = berth::netlog_append(paths, {
                                                     .host = "api.github.com",
                                                     .ip = "140.82.121.6",
                                                     .port = 443,
                                                     .purpose = "https-get",
                                                     .note = "403 rate limit",
                                                 });
  CHECK(first.is_ok());

  const auto second =
      berth::netlog_append(paths, {
                                      .host = "evil.example.com",
                                      .ip = "",
                                      .port = 443,
                                      .purpose = "https-get",
                                      .note = "not on allowlist",
                                      .allowed = false,
                                  });
  CHECK(second.is_ok());

  const auto raw = berth::read_file(paths.net_log);
  CHECK(raw.is_ok());
  CHECK(raw.value().find(
            "api.github.com 140.82.121.6 port=443 purpose=https-get allowed") !=
        std::string::npos);
  CHECK(raw.value().find("evil.example.com") != std::string::npos);
  CHECK(raw.value().find("DENIED") != std::string::npos);
  CHECK(std::count(raw.value().begin(), raw.value().end(), '\n') == 2);
  CHECK(std::filesystem::status(paths.net_log).permissions() ==
        berth::kPermPrivate);
}

TEST(netlog_newlines_are_sanitized) {
  const TempDir tmp;
  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  paths.net_log = paths.state_dir / "net.log";

  CHECK(berth::netlog_append(paths,
                             {
                                 .host = "github.com",
                                 .ip = "",
                                 .port = 443,
                                 .purpose = "probe",
                                 .note = "line1\nline2\rinjected",
                             })
            .is_ok());

  const auto raw = berth::read_file(paths.net_log);
  CHECK(raw.is_ok());
  CHECK(std::count(raw.value().begin(), raw.value().end(), '\n') == 1);
  CHECK(raw.value().find("line1 line2 injected") != std::string::npos);
}
