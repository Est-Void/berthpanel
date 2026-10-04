// CLI entry point: dispatches flags and one-shot subcommands
// (--version, --show-endpoints, __egress-probe now; __deploy / __cleanup /
// __serve-static land in later stages). Owns nothing beyond dispatch:
// real work lives in modules. Layering: berth is linked but tui/ does
// not exist yet, and when it does, this file stays the only bridge.

#include <csignal>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "berth/egress.h"
#include "berth/lock.h"
#include "netlog.h"
#include "paths.h"
#include "util/result.h"
#include "version.h"

namespace {

int fail(const std::string_view message) {
  std::cerr << "berthpanel: " << message << "\n";
  return 1;
}

int show_endpoints() {
  std::cout << "kAllowedEndpoints (§6.1) — every outbound request passes\n"
               "this exact-match table and is appended to net.log. The\n"
               "table and its test change in the same commit.\n\n";
  for (const std::string_view host : berth::allowed_endpoints()) {
    std::cout << "  " << host << "\n";
  }
  std::cout << "\n  loopback (127.0.0.1, ::1, localhost) is permitted for\n"
               "  local health checks only, never for API calls.\n";
  return 0;
}

int egress_probe() {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  std::cout << "probing " << berth::allowed_endpoints().size()
            << " allowlisted endpoints (per-host timeout 6s)...\n\n";
  const berth::Result<std::vector<berth::EgressProbeResult>> results =
      berth::run_egress_probe(paths.value(), nullptr,
                              std::chrono::milliseconds(6000));
  if (results.is_err())
    return fail(results.error().ctx());
  std::cout << berth::render_egress_report(results.value());
  for (const berth::EgressProbeResult& r : results.value()) {
    if (!r.reachable)
      return 1;
  }
  return 0;
}

int run(const std::span<const char* const> args) {
  if (!args.empty()) {
    // One-shot subcommands must be the first argument.
    const std::string_view first(args[0]);
    if (first == "__egress-probe")
      return egress_probe();
    // Test hook for the SIGKILL flock scenario (stage 4.3); not a user
    // command, documented in berth/lock.h.
    if (first == "__test-lock-child" && args.size() == 2) {
      return berth::lock_test_child(args[1]);
    }
  }
  for (const char* const arg : args) {
    const std::string_view a(arg);
    if (a == "--version") {
      std::cout << "berthpanel " << BERTH_VERSION << " (" << BERTH_COMMIT
                << ")\n";
      return 0;
    }
    if (a == "--show-endpoints")
      return show_endpoints();
  }
  std::cerr << "usage: berthpanel [--version] [--show-endpoints]\n"
               "       berthpanel __egress-probe\n";
  return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Build tools inherit the disposition; git and friends handle EPIPE
  // themselves. Without this, an early-closed pipe kills the panel.
  ::signal(SIGPIPE, SIG_IGN);

  const char* const* first = argc > 0 ? argv + 1 : nullptr;
  const std::span<const char* const> args(
      first, argc > 0 ? static_cast<std::size_t>(argc - 1) : std::size_t{0});
  return run(args);
}
