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

#include <stdlib.h>

#include <unistd.h>

#include "berth/cancel.h"
#include "berth/catalog.h"
#include "berth/deploy.h"
#include "berth/detect.h"
#include "berth/gitclone.h"
#include "berth/lock.h"
#include "berth/net/egress.h"
#include "berth/recipes.h"
#include "berth/sandbox.h"
#include "berth/toolcheck.h"
#include "berth/units.h"
#include "berth/userrecipes.h"
#include "berth/validate.h"
#include "core/json.h"
#include "core/names.h"
#include "core/netlog.h"
#include "core/paths.h"
#include "core/state.h"
#include "util/argv.h"
#include "util/fs.h"
#include "util/result.h"
#include "util/tmpdir.h"
#include "version.h"

namespace {

int fail(const std::string_view message) {
  std::cerr << berth::kAppName << ": " << message << "\n";
  return 1;
}

bool contains_flag(const std::span<const char* const>& args,
                   const std::string_view flag) {
  const std::vector<std::string> argv(args.begin(), args.end());
  return berth::has_arg(argv, flag);
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
                   const std::string_view port_arg,
                   const bool upgrade_confirmed) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  // One deploy path for curated and BYO recipes: user recipes override
  // builtin ones (berth/catalog.h).
  const berth::Result<berth::Recipe> recipe =
      berth::resolve_recipe(paths.value(), service);
  if (recipe.is_err())
    return fail(recipe.error().ctx());

  // Upgrade gate (stage 4.22): redeploying on a NEW SHA is an upgrade and
  // needs explicit confirmation; the old and new pins are shown first.
  // A same-SHA redeploy is a repair (e.g. after a registry failure) and
  // runs without ceremony.
  const berth::Result<std::optional<berth::ServiceRecord>> existing =
      berth::registry_get(paths.value(), recipe.value().service_name);
  if (existing.is_err())
    return fail(existing.error().ctx());
  bool is_upgrade = false;
  if (existing.value().has_value() &&
      existing.value()->commit != recipe.value().commit) {
    if (!upgrade_confirmed) {
      std::cerr << "refusing to upgrade " << recipe.value().service_name
                << " without confirmation:\n"
                << "  registered: " << existing.value()->commit.substr(0, 7)
                << " (" << existing.value()->repo << ")\n"
                << "  recipe:     " << recipe.value().commit.substr(0, 7)
                << " (" << recipe.value().owner_repo() << ")\n"
                << "rerun with --upgrade to confirm\n";
      return 2;
    }
    is_upgrade = true;
  }

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
  if (is_upgrade) {
    std::cout << "upgraded " << outcome.value().service_name << ": "
              << existing.value()->commit.substr(0, 7) << " -> "
              << recipe.value().commit.substr(0, 7) << "\n";
  } else {
    std::cout << "deployed " << outcome.value().service_name;
    if (existing.value().has_value())
      std::cout << " (repair)";
    std::cout << "\n";
  }
  std::cout << "  port: " << outcome.value().port << "\n"
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

int recipe_list_command() {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<std::vector<berth::Recipe>> recipes =
      berth::user_recipes_load(paths.value());
  if (recipes.is_err())
    return fail(recipes.error().ctx());
  std::cout << berth::render_user_recipes_list(recipes.value());
  return 0;
}

int recipe_add_command(const std::string_view file_arg, const bool force) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const std::filesystem::path file(file_arg);
  const berth::Result<std::string> raw = berth::read_file(file);
  if (raw.is_err())
    return fail(raw.error().ctx());

  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(raw.value());
  } catch (const nlohmann::json::exception& e) {
    return fail("recipe-add: " + file.string() + ": " + e.what());
  }
  const berth::Result<berth::Recipe> parsed = berth::recipe_from_json(doc);
  if (parsed.is_err())
    return fail("recipe-add: " + file.string() + ": " + parsed.error().ctx());

  const berth::Result<std::filesystem::path> stored =
      berth::user_recipe_store(paths.value(), parsed.value(), force);
  if (stored.is_err())
    return fail(stored.error().ctx());
  std::cout << "stored " << parsed.value().service_name << " -> "
            << stored.value().string() << "\n";
  return 0;
}

int recipe_remove_command(const std::string_view service) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<bool> removed =
      berth::user_recipe_remove(paths.value(), service);
  if (removed.is_err())
    return fail(removed.error().ctx());
  if (!removed.value()) {
    std::cout << "no user recipe named '" << service << "'\n";
    return 1;
  }
  std::cout << "removed " << service << "\n";
  return 0;
}

int cleanup_command(const bool install, const bool quiet) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  berth::RealDeployOps ops;
  const berth::Result<berth::ReconcileReport> report =
      berth::reconcile_interrupted(paths.value(), ops, nullptr);
  if (report.is_err())
    return fail(report.error().ctx());
  if (!quiet)
    std::cout << berth::render_reconcile_report(report.value());

  if (install) {
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
      return fail("cleanup --install: cannot resolve /proc/self/exe");
    self[n] = '\0';
    const std::filesystem::path binary(self);
    const berth::Result<std::filesystem::path> timer =
        berth::write_cleanup_units(paths.value(), binary);
    if (timer.is_err())
      return fail(timer.error().ctx());
    const berth::Result<void> reloaded = berth::systemctl_daemon_reload();
    if (reloaded.is_err())
      return fail(reloaded.error().ctx());
    const berth::Result<void> enabled = berth::systemctl_user(
        {"enable", "--now", std::string(berth::kCleanupUnitStem) + ".timer"});
    if (enabled.is_err())
      return fail(enabled.error().ctx());
    std::cout << "cleanup timer installed: " << timer.value().string() << "\n";
  }

  // Failures to reconcile are loud but partial success still counts:
  // the report names what stayed behind for the next pass.
  return report.value().failed.empty() ? 0 : 1;
}

int recipe_draft_command(const std::string_view url_arg,
                         const std::string_view ref_arg, const bool apply,
                         const bool force) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  // Front-door egress policy (§6.1, stage 4.23): https, host exactly
  // github.com — everything else is refused before any byte moves.
  const berth::Result<std::string> owner_repo =
      berth::github_owner_repo_from_url(std::string(url_arg));
  if (owner_repo.is_err())
    return fail(owner_repo.error().ctx());
  const std::size_t slash = owner_repo.value().find('/');
  const std::string owner = owner_repo.value().substr(0, slash);
  const std::string name = owner_repo.value().substr(slash + 1);

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  // The pin: an explicit 40-hex SHA is taken as-is, anything else is
  // resolved through the API NOW — repos/<o>/<r> for the default branch,
  // commits/<branch> for its head, both GETs net.log-logged (§6.6 anti-TOFU
  // — the draft freezes one commit, it never tracks a branch). Public
  // repos only: no token is attached (§6.3).
  std::string commit(ref_arg);
  if (commit.empty() || !berth::validate_commit_sha(commit).is_ok()) {
    const std::string ref(ref_arg);
    std::cout << "resolving " << owner_repo.value() << "@"
              << (ref.empty() ? "default branch" : ref) << " via api...\n";
    const berth::Result<std::string> resolved = berth::resolve_head_sha(
        paths.value(), owner_repo.value(), ref, &cancel_flag);
    if (resolved.is_err())
      return fail(resolved.error().ctx());
    commit = resolved.value();
  }

  // Scratch checkout with a random suffix (§9); removed however detection
  // ends. Detection only READS this tree (detect.h).
  const berth::Result<berth::TempDir> scratch = berth::TempDir::create("draft");
  if (scratch.is_err())
    return fail(scratch.error().ctx());
  const std::filesystem::path dir = scratch.value().path();

  std::cout << "cloning " << owner_repo.value() << "@" << commit.substr(0, 7)
            << "...\n";
  const berth::Result<void> cloned = berth::clone_pinned({
      .url = "https://" + std::string(berth::kTokenHost) + "/" +
             owner_repo.value() + ".git",
      .commit = commit,
      .dest_dir = dir,
      .timeout = std::chrono::milliseconds(600000),
      .cancel = &cancel_flag,
  });
  if (cloned.is_err())
    return fail(cloned.error().ctx());

  // §6.1: a git clone is an outbound request to github.com that does not
  // run through httpclient, so the attempt is logged here — before it
  // happens, so the line exists even if the process dies mid-clone.
  const std::string note = "draft clone " + owner_repo.value() + "@" + commit;
  const berth::Result<void> net_logged =
      berth::netlog_append(paths.value(), {.host = berth::kTokenHost,
                                           .ip = "",
                                           .port = 443,
                                           .purpose = "git-fetch",
                                           .note = note,
                                           .allowed = true});
  if (net_logged.is_err())
    return fail(net_logged.error().ctx());

  const berth::DetectRequest detect{
      .dir = dir, .owner = owner, .name = name, .commit = commit};
  const berth::Result<berth::RecipeDraft> detected =
      berth::detect_recipe(detect);
  if (detected.is_err())
    return fail(detected.error().ctx());
  const berth::Recipe& draft = detected.value().recipe;

  // The draft is ALWAYS saved, untouched or not: what can be deployed is
  // what is on disk (reproducibility, plan 4.21). Overwriting an existing
  // recipe needs --force, like __recipe-add.
  const berth::Result<std::filesystem::path> stored =
      berth::user_recipe_store(paths.value(), draft, force);
  if (stored.is_err())
    return fail(stored.error().ctx());

  // toolcheck verifies the detected tools; a missing tool is a warning —
  // install it, then deploy. The draft deploys through the same preflight
  // gate either way.
  const berth::Result<std::vector<berth::ToolStatus>> tools =
      berth::check_tools(draft.tools, &cancel_flag);
  if (tools.is_err())
    return fail(tools.error().ctx());

  std::cout << "draft " << draft.service_name << " (" << draft.language
            << ", " << draft.res.memory_max << "/" << draft.res.tasks_max
            << ")\n"
            << "  saved: " << stored.value().string() << "\n"
            << "  start: " << draft.start_cmd << "\n"
            << "  tools:\n"
            << berth::render_tool_report(tools.value());
  for (const std::string& warning : detected.value().warnings) {
    std::cout << "  warning: " << warning << "\n";
  }
  std::cout << "the heuristics may be wrong — edit " << stored.value().string()
            << ", then:\n"
            << "  berthpanel __recipe-add " << stored.value().string()
            << " --force\n";
  if (apply) {
    std::cout << "deploy it with:\n"
              << "  berthpanel __deploy " << draft.service_name << "\n";
  }
  return 0;
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
    if (first == "__recipe-list")
      return recipe_list_command();
    if (first == "__recipe-add" && args.size() == 2)
      return recipe_add_command(args[1], false);
    if (first == "__recipe-add" && args.size() == 3 &&
        std::string_view(args[2]) == "--force")
      return recipe_add_command(args[1], true);
    if (first == "__recipe-remove" && args.size() == 2)
      return recipe_remove_command(args[1]);
    if (first == "__recipe-draft" && args.size() >= 2) {
      std::string_view ref;
      bool apply = false;
      bool force = false;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (std::string_view(args[i]) == "--apply") {
          apply = true;
        } else if (std::string_view(args[i]) == "--force") {
          force = true;
        } else if (ref.empty()) {
          ref = args[i];
        }
      }
      return recipe_draft_command(args[1], ref, apply, force);
    }
    if (first == "__deploy" && args.size() >= 2) {
      const std::string_view service = args[1];
      std::string_view port_arg;
      bool upgrade = false;
      for (std::size_t i = 2; i < args.size(); ++i) {
        if (std::string_view(args[i]) == "--upgrade") {
          upgrade = true;
        } else if (port_arg.empty()) {
          port_arg = args[i];
        }
      }
      return deploy_command(service, port_arg, upgrade);
    }
    if (first == "__cleanup") {
      return cleanup_command(contains_flag(args, "--install"),
                             contains_flag(args, "--quiet"));
    }
    // Test hook for the SIGKILL flock scenario (stage 4.3); not a user
    // command, documented in berth/lock.h.
    if (first == "__test-lock-child" && args.size() == 2) {
      return berth::lock_test_child(args[1]);
    }
  }
  for (const char* const arg : args) {
    const std::string_view a(arg);
    if (a == "--version") {
      std::cout << berth::kAppName << " " << BERTH_VERSION << " ("
                << BERTH_COMMIT << ")\n";
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
               "       berthpanel __recipe-list | __recipe-add <file.json> "
               "[--force] | "
               "__recipe-remove <service>\n"
               "       berthpanel __recipe-draft <github_url> [ref] [--apply] "
               "[--force]\n"
               "       berthpanel __deploy <service_name> [port] [--upgrade]\n"
               "       berthpanel __cleanup [--install] [--quiet]\n";
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
