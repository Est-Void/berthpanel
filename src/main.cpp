// CLI entry point: dispatches flags and one-shot subcommands
// (--version, --show-endpoints, __egress-probe now; __deploy / __cleanup /
// __serve-static land in later stages). Owns nothing beyond dispatch:
// real work lives in modules. Layering: berth is linked but tui/ does
// not exist yet, and when it does, this file stays the only bridge.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "berth/catalog.h"
#include "berth/cancel.h"
#include "berth/deploy.h"
#include "berth/egress.h"
#include "berth/lock.h"
#include "berth/sandbox.h"
#include "netlog.h"
#include "paths.h"
#include "state.h"
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

int deploy_command(const std::string_view service,
                   const std::string_view port_arg) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<berth::Recipe> recipe = berth::builtin_recipe(service);
  if (recipe.is_err())
    return fail(recipe.error().ctx());

  // Ctrl-C / SIGTERM flip this flag; every long operation polls it, so a
  // cancelled deploy unwinds with its journal entry kept for __cleanup
  // (§6.7). A deploy without exit signalling must not start.
  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  std::uint16_t forced_port = 0;
  if (!port_arg.empty()) {
    forced_port =
        static_cast<std::uint16_t>(std::atoi(std::string(port_arg).c_str()));
  }

  berth::RealDeployOps ops;
  berth::DeployOptions options;
  options.cancel = &cancel_flag;
  const berth::Result<berth::DeployOutcome> outcome = berth::run_deploy(
      paths.value(), recipe.value(), forced_port, ops, options);
  if (outcome.is_err()) {
    std::cerr << "deploy failed:\n" << outcome.error().ctx() << "\n";
    return 1;
  }
  std::cout << "deployed " << outcome.value().service_name << " on port "
            << outcome.value().port << "\n"
            << "  unit: " << outcome.value().unit_path.string() << "\n"
            << "  work: " << outcome.value().work_dir.string() << "\n";
  return 0;
}

int services_command() {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<std::vector<berth::ServiceRecord>> listed =
      berth::registry_list(paths.value());
  if (listed.is_err())
    return fail(listed.error().ctx());
  if (listed.value().empty()) {
    std::cout << "no services registered\n";
    return 0;
  }
  for (const berth::ServiceRecord& record : listed.value()) {
    std::cout << "  " << record.service_name << "  port " << record.port << "  "
              << record.commit.substr(0, 7) << "  " << record.display_name
              << "\n";
  }
  return 0;
}

int verify_sandbox_command() {
  const berth::Result<berth::SandboxAudit> audit = berth::verify_sandbox();
  if (audit.is_err())
    return fail(audit.error().ctx());
  std::cout << berth::render_sandbox_audit(audit.value());
  return audit.value().full_grade() ? 0 : 1;
}

int run(const std::span<const char* const> args) {
  if (!args.empty()) {
    // One-shot subcommands must be the first argument.
    const std::string_view first(args[0]);
    if (first == "__egress-probe")
      return egress_probe();
    if (first == "__catalog") {
      std::cout << berth::render_catalog_list();
      return 0;
    }
    if (first == "__services")
      return services_command();
    if (first == "__deploy" && args.size() >= 2)
      return deploy_command(args[1], args.size() >= 3 ? args[2] : "");
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
    if (a == "--verify-sandbox")
      return verify_sandbox_command();
  }
  std::cerr << "usage: berthpanel [--version] [--show-endpoints] "
               "[--verify-sandbox]\n"
               "       berthpanel __egress-probe | __catalog | __services\n"
               "       berthpanel __deploy <service_name> [port]\n";
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
