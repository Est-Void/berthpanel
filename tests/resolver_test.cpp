// Tests for resolver.h (stage 3.3): the pure DoH parser offline, live
// resolution strictly opt-in via BERTHPANEL_RUN_NETWORK_TESTS (never in
// CI, dev.md §9).

#include "berth/resolver.h"

#include <string>

#include "flags.h"
#include "runner.h"
#include "test_env.h"

using berth::parse_doh_answers;

TEST(doh_parse_a_and_aaaa) {
  const std::string body = R"({
    "Status": 0,
    "Answer": [
      {"name": "github.com", "type": 5, "data": "example.com."},
      {"name": "github.com", "type": 1, "TTL": 60, "data": "140.82.121.4"},
      {"name": "github.com", "type": 28, "TTL": 60, "data": "2606:50c0:8000::153"}
    ]
  })";
  const auto r = parse_doh_answers(body);
  CHECK(r.is_ok());
  CHECK(r.value().size() == 2);  // CNAME skipped, A + AAAA kept
  CHECK(r.value()[0].ip == "140.82.121.4");
  CHECK(!r.value()[0].ipv6);
  CHECK(r.value()[1].ip == "2606:50c0:8000::153");
  CHECK(r.value()[1].ipv6);
}

TEST(doh_parse_rejects_loudly) {
  CHECK(parse_doh_answers("not json").is_err());
  CHECK(parse_doh_answers(R"({"NoStatus": 1})").is_err());
  CHECK(parse_doh_answers(R"({"Status": 3})").is_err());  // NXDOMAIN
  CHECK(parse_doh_answers(R"({"Status": 0})").is_err());
  CHECK(parse_doh_answers(R"({"Status": 0, "Answer": []})").is_err());
  // Only a CNAME in Answer: no usable address -> loud error, not a skip.
  CHECK(parse_doh_answers(
            R"({"Status": 0, "Answer": [{"type": 5, "data": "x."}]})")
            .is_err());
  // Address-shaped records failing inet_pton are dropped.
  CHECK(parse_doh_answers(
            R"({"Status": 0, "Answer": [{"type": 1, "data": "999.999.1.1"}]})")
            .is_err());
}

// ---------------------------------------------------------------------------
// Live network — opt-in only, never in CI (dev.md §9).
// ---------------------------------------------------------------------------

namespace {

bool network_tests_enabled() {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_NETWORK_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_NETWORK_TESTS");
  return enabled.is_ok() && enabled.value().value_or(false);
}

}  // namespace

TEST(resolver_live_resolution_opt_in) {
  if (!network_tests_enabled()) {
    return;  // skipped: set BERTHPANEL_RUN_NETWORK_TESTS=true to enable
  }
  berth::Paths paths;
  const TempDir tmp;
  paths.state_dir = tmp.path() / "state";
  paths.net_log = paths.state_dir / "net.log";

  const auto r = berth::resolve_host(
      "github.com", std::chrono::milliseconds(15000), paths, nullptr);
  CHECK(r.is_ok());
  CHECK(!r.value().empty());
}
