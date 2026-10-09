#pragma once

// src/berth/selftest.h — --selftest (plan 7.1): an end-to-end check against
// the LIVE systemd user manager, loopback only, no network.
//
// Why a rendered unit file and not a `systemd-run` transient unit: the deploy
// path this validates IS render_unit() -> write_unit() -> daemon-reload ->
// `systemctl --user enable --now`. A transient run would exercise a different
// mechanism and prove less. The manager's unit namespace is shared with every
// other unit the user runs, so the test's unit gets a random-suffixed name
// (validated by validate_unit_name) that is checked free — no unit file and no
// registry entry — before it is used, and it is removed on every exit path.
//
// The service work dir is a random-suffixed scratch dir under the REAL data
// dir, not /tmp: the unit carries PrivateTmp=yes, so a /tmp work dir is
// invisible inside the unit's mount namespace (verified: the unit fails with
// 226/NAMESPACE). Deployed services live under paths.services_dir for the same
// reason. Temp-XDG does NOT isolate units from the manager either — the unit
// file is written where the manager actually looks (paths.units_dir).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "units.h"
#include "util/result.h"

namespace berth {

struct SelftestStep {
  std::string name;
  bool ok = false;
  std::string detail;
};

struct SelftestReport {
  std::vector<SelftestStep> steps;
  bool leftovers_free = false;
  [[nodiscard]] bool ok() const;
};

// --- pure, testable without a user manager ------------------------------

// "berthpanel-selftest-<suffix>". The suffix must be [a-z0-9-] for the result
// to pass validate_unit_name.
[[nodiscard]] std::string selftest_unit_name(std::string_view suffix);

// 8 hex characters from std::random_device (a fixed /tmp path is the §9 trap;
// a fixed unit name would be the same trap against the manager).
[[nodiscard]] Result<std::string> selftest_random_suffix();

// The exact HTTP/1.0 request the E2E sends over loopback.
[[nodiscard]] std::string selftest_http_request(std::string_view path);

// -1 when the buffer is not an "HTTP/1.x <code>" status line.
[[nodiscard]] int parse_http_status(std::string_view wire);
[[nodiscard]] std::string parse_http_body(std::string_view wire);

// Pure UnitSpec for the loopback service (the berthpanel binary in
// __serve-static mode); render_unit() turns it into the actual text.
[[nodiscard]] Result<UnitSpec> selftest_unit_spec(
    const std::string& unit_name, const std::filesystem::path& binary,
    const std::filesystem::path& root, std::uint16_t port);

// Ok(true) when the name is neither a unit file nor a registry entry;
// Ok(false) when it is taken; Err when that cannot be determined (an
// unreadable registry must never read as "free").
[[nodiscard]] Result<bool> selftest_name_is_free(const Paths& paths,
                                                 std::string_view name);

// --- the live E2E -------------------------------------------------------

// `binary` must be an absolute path to a berthpanel binary (the CLI passes
// /proc/self/exe). The report names every step; `ok()` is false if any step
// failed or a leftover remained. Cleanup is attempted on every path,
// including a set cancel flag; the flag is only polled, never cleared.
[[nodiscard]] Result<SelftestReport> run_selftest(
    const Paths& paths, const std::filesystem::path& binary,
    std::atomic<bool>* cancel);

[[nodiscard]] std::string render_selftest_report(const SelftestReport& report);

}  // namespace berth
