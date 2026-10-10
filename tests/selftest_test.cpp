// Tests for selftest.h (plan 7.1): the pure pieces — name generation, the
// HTTP request/reply parsing and the rendered unit spec — plus an opt-in live
// E2E against the real systemd user manager. The live part never runs in CI.

#include "berth/selftest.h"

#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <mutex>
#include <string>

#include "berth/units.h"
#include "core/atomic.h"
#include "core/flags.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/env.h"
#include "util/fs.h"

using berth::parse_http_body;
using berth::parse_http_status;
using berth::selftest_http_request;
using berth::selftest_random_suffix;
using berth::selftest_unit_name;
using berth::selftest_unit_spec;

namespace {

berth::Paths selftest_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.data_dir = root / "data" / "berthpanel";
  p.units_dir = root / "config" / "systemd" / "user";
  p.state_json = p.state_dir / "state.json";
  return p;
}

berth::ServiceRecord record_named(const std::string& name) {
  berth::ServiceRecord r;
  r.service_name = name;
  r.display_name = "x";
  r.description = "x";
  r.repo = "o/r";
  r.commit = "abc";
  r.port = 1234;
  return r;
}

}  // namespace

TEST(selftest_unit_name_is_well_formed) {
  const std::string name = selftest_unit_name("deadbeef");
  CHECK(name == "berthpanel-selftest-deadbeef");
  CHECK(berth::validate_unit_name(name));
}

TEST(selftest_random_suffix_is_hex_and_valid_as_a_name) {
  for (int i = 0; i < 128; ++i) {
    const auto suffix = selftest_random_suffix();
    CHECK(suffix.is_ok());
    CHECK(suffix.value().size() == 8);
    for (const char c : suffix.value()) {
      const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      CHECK(hex);
    }
    CHECK(berth::validate_unit_name(selftest_unit_name(suffix.value())));
  }
}

TEST(selftest_http_request_is_a_loopback_get) {
  const std::string request = selftest_http_request("/index.html");
  CHECK(request.rfind("GET /index.html HTTP/1.0\r\n", 0) == 0);
  CHECK(request.find("\r\n\r\n") != std::string::npos);
  CHECK(request.find("Host:") != std::string::npos);
}

TEST(selftest_http_reply_parsing) {
  const std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  CHECK(parse_http_status(wire) == 200);
  CHECK(parse_http_body(wire) == "hello");

  CHECK(parse_http_status("HTTP/1.1 404 Not Found\r\n\r\n") == 404);
  CHECK(parse_http_status("not http") == -1);
  CHECK(parse_http_status("HTTP/1.1 xx") == -1);
  CHECK(parse_http_status("HTTP/1.") == -1);
  CHECK(parse_http_body("no separator") == "");
}

TEST(selftest_unit_spec_renders_a_loopback_service) {
  const auto spec = selftest_unit_spec("berthpanel-selftest-abcd", "/bin/true",
                                       "/tmp/selftest-root", 43210);
  CHECK(spec.is_ok());
  const auto unit = berth::render_unit(spec.value());
  CHECK(unit.is_ok());
  CHECK(unit.value().find("ExecStart=") != std::string::npos);
  CHECK(unit.value().find("\"__serve-static\"") != std::string::npos);
  CHECK(unit.value().find("\"--listen\" \"127.0.0.1:43210\"") !=
        std::string::npos);
  CHECK(unit.value().find("MemoryMax=128M") != std::string::npos);
  CHECK(unit.value().find("TasksMax=64") != std::string::npos);
  CHECK(unit.value().find("ReadWritePaths=/tmp/selftest-root") !=
        std::string::npos);
  CHECK(unit.value().find("PrivateTmp=yes") != std::string::npos);

  // A relative binary or root would make the unit untraceable/wrong.
  CHECK(selftest_unit_spec("berthpanel-selftest-abcd", "relative", "/tmp/r", 1)
            .is_err());
  CHECK(selftest_unit_spec("berthpanel-selftest-abcd", "/bin/true", "rel", 1)
            .is_err());
}

TEST(selftest_name_free_checks_unit_file_and_registry) {
  const TempDir tmp;
  const berth::Paths p = selftest_paths(tmp.path());

  const auto free = berth::selftest_name_is_free(p, "berthpanel-selftest-aaaa");
  CHECK(free.is_ok());
  CHECK(free.value());

  // An invalid name can never be used.
  CHECK(berth::selftest_name_is_free(p, "not-prefixed").is_err());

  // A registered name is not free.
  CHECK(berth::registry_register(p, record_named("berthpanel-selftest-bbbb"))
            .is_ok());
  const auto taken =
      berth::selftest_name_is_free(p, "berthpanel-selftest-bbbb");
  CHECK(taken.is_ok());
  CHECK(!taken.value());

  // An existing unit file is not free either.
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  const auto wrote =
      berth::atomic_write(p.units_dir / "berthpanel-selftest-cccc.service",
                          "[Unit]\n", berth::kPermShared);
  CHECK(wrote.is_ok());
  const auto file_taken =
      berth::selftest_name_is_free(p, "berthpanel-selftest-cccc");
  CHECK(file_taken.is_ok());
  CHECK(!file_taken.value());
}

// The live E2E is opt-in and needs a real berthpanel binary (the test process
// is the test runner, not a server). Point BERTHPANEL_SELFTEST_BINARY at one,
// or build/debug/berthpanel; otherwise the test is a no-op.
TEST(selftest_live_end_to_end_opt_in) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  if (!(enabled.is_ok() && enabled.value().value_or(false)))
    return;

  const char* configured = ::getenv("BERTHPANEL_SELFTEST_BINARY");
  std::filesystem::path binary =
      configured != nullptr ? std::filesystem::path(configured)
                            : std::filesystem::path("build/debug/berthpanel");
  if (!binary.is_absolute())
    binary = std::filesystem::current_path() / binary;
  if (::access(binary.c_str(), X_OK) != 0)
    return;  // no berthpanel binary available: cannot run the E2E here

  const auto paths = berth::paths_from_env();
  CHECK(paths.is_ok());
  const auto report = berth::run_selftest(paths.value(), binary, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().ok());
}
