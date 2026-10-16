#pragma once

// End-to-end against the LIVE systemd user manager, loopback only, no network.
// A rendered unit file, not a systemd-run transient unit: the deploy path is
// render_unit() -> write_unit() -> daemon-reload -> enable --now.
// The name is random-suffixed and checked free: the namespace is shared. The
// work dir is under the REAL data dir: PrivateTmp=yes hides /tmp (226/NS).

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

[[nodiscard]] std::string selftest_unit_name(std::string_view suffix);

// 8 hex chars: a fixed unit name is the §9 trap against the shared manager.
[[nodiscard]] Result<std::string> selftest_random_suffix();

[[nodiscard]] std::string selftest_http_request(std::string_view path);

[[nodiscard]] int parse_http_status(std::string_view wire);
[[nodiscard]] std::string parse_http_body(std::string_view wire);

[[nodiscard]] Result<UnitSpec> selftest_unit_spec(
    const std::string& unit_name, const std::filesystem::path& binary,
    const std::filesystem::path& root, std::uint16_t port);

// Ok(false) when taken; Err when undeterminable, never "free".
[[nodiscard]] Result<bool> selftest_name_is_free(const Paths& paths,
                                                 std::string_view name);

[[nodiscard]] Result<SelftestReport> run_selftest(
    const Paths& paths, const std::filesystem::path& binary,
    std::atomic<bool>* cancel);

[[nodiscard]] std::string render_selftest_report(const SelftestReport& report);

}
