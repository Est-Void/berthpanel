// berth must not depend on tui — this file is the only bridge (§5).

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <stdlib.h>

#include <unistd.h>

#include "analyzer/ports.h"
#include "berth/backup.h"
#include "berth/cancel.h"
#include "berth/catalog.h"
#include "berth/deploy.h"
#include "berth/detect.h"
#include "berth/gitclone.h"
#include "berth/journal.h"
#include "berth/lock.h"
#include "berth/net/egress.h"
#include "berth/net/github_api.h"
#include "berth/proxy.h"
#include "berth/recipe_edit.h"
#include "berth/recipes.h"
#include "berth/repair.h"
#include "berth/secret_ops.h"
#include "berth/secrets.h"
#include "berth/sandbox.h"
#include "berth/selftest.h"
#include "berth/serve.h"
#include "berth/service_ops.h"
#include "berth/status.h"
#include "berth/toolcheck.h"
#include "berth/units.h"
#include "berth/userrecipes.h"
#include "berth/validate.h"
#include "berth/wizard.h"
#include "core/json.h"
#include "core/jsonout.h"
#include "core/names.h"
#include "core/netlog.h"
#include "core/paths.h"
#include "core/state.h"
#include "tui/app.h"
#include "tui/view_deploy.h"
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

void print_usage() {
  std::cerr
      << "usage: berthpanel                       # the panel (needs a "
         "terminal)\n"
         "       berthpanel [--version] [--show-endpoints] "
         "[--verify-sandbox] [--selftest]\n"
         "       berthpanel __egress-probe | __catalog [--json] | "
         "__services [--json]\n"
         "       berthpanel __recipe-list [--json] | "
         "__recipe-add <file.json> "
         "[--force] | "
         "__recipe-remove <service>\n"
         "       berthpanel __recipe-edit <service> [--start-cmd <tmpl>] "
         "[--bind 127.0.0.1] [--port <n>]\n"
         "              [--memory <512M>] [--tasks <n>] [--health <path>] "
         "[--name <display>] [--description <text>]\n"
         "       berthpanel __recipe-draft <github_url> [ref] [--apply] "
         "[--force]\n"
         "       berthpanel __secret-set <service> <KEY>   (value on stdin)\n"
         "       berthpanel __secret-keys <service>\n"
         "       berthpanel __deploy <service_name> [port] [--upgrade] "
         "[--json]\n"
         "       berthpanel __stacks [--json] | "
         "__deploy-stack <stack> [--json]\n"
         "       berthpanel proxy add <service> <domain> | proxy list | "
         "proxy remove <domain>\n"
         "       berthpanel __cleanup [--install] [--quiet]\n"
         "       berthpanel __repair [--json]\n"
         "       berthpanel __serve-static <root> [--listen "
         "127.0.0.1:<port>]\n"
         "       berthpanel __serve-status [--listen "
         "127.0.0.1:<port>]\n"
         "       berthpanel backup <service> [<archive>] | "
         "restore <service> <archive> [--force]\n";
}

int deploy_command(const std::string_view service,
                   const std::string_view port_arg,
                   const bool upgrade_confirmed, const bool json) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<berth::Recipe> recipe =
      berth::resolve_recipe(paths.value(), service);
  if (recipe.is_err())
    return fail(recipe.error().ctx());

  // A different SHA is an upgrade needing --upgrade; the same SHA is a repair.
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

  // No exit signalling, no deploy: a cancel unwinds, journal entry kept.
  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  std::uint16_t forced_port = 0;
  if (!port_arg.empty()) {
    // The ONE port parser: atoi() wrapped "70000", read "abc" as auto (§11).
    const berth::Result<std::uint16_t> parsed = tui::parse_port_field(port_arg);
    if (parsed.is_err())
      return fail(parsed.error().ctx());
    forced_port = parsed.value();
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
  if (json) {
    nlohmann::json doc = berth::json_document("deploy");
    doc["service_name"] = outcome.value().service_name;
    doc["action"] =
        is_upgrade ? "upgraded"
                   : (existing.value().has_value() ? "repair" : "deployed");
    doc["repo"] = recipe.value().owner_repo();
    doc["commit"] = recipe.value().commit;
    doc["port"] = outcome.value().port;
    doc["unit"] = outcome.value().unit_path.string();
    doc["work_dir"] = outcome.value().work_dir.string();
    std::cout << berth::json_text(doc);
    return 0;
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

int stacks_command(const bool json) {
  if (json) {
    std::cout << berth::json_text(berth::stacks_json());
    return 0;
  }
  std::cout << berth::render_stack_list();
  return 0;
}

int deploy_stack_command(const std::string_view stack_name, const bool json) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<berth::Stack> stack =
      berth::builtin_stack(stack_name);
  if (stack.is_err())
    return fail(stack.error().ctx());

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  berth::RealDeployOps ops;
  berth::DeployOptions options;
  options.cancel = &cancel_flag;
  const berth::Result<berth::StackDeployOutcome> outcome =
      berth::run_stack_deploy(paths.value(), stack.value(), ops, options);
  if (outcome.is_err()) {
    std::cerr << "stack deploy failed:\n" << outcome.error().ctx() << "\n";
    return 1;
  }
  if (json) {
    nlohmann::json doc = berth::json_document("deploy-stack");
    doc["stack"] = outcome.value().stack_name;
    nlohmann::json members = nlohmann::json::array();
    for (const berth::DeployOutcome& member : outcome.value().members) {
      members.push_back({{"service_name", member.service_name},
                         {"port", member.port},
                         {"unit", member.unit_path.string()},
                         {"work_dir", member.work_dir.string()}});
    }
    doc["members"] = std::move(members);
    std::cout << berth::json_text(doc);
    return 0;
  }
  std::cout << "deployed stack " << outcome.value().stack_name << "\n";
  for (const berth::DeployOutcome& member : outcome.value().members) {
    std::cout << "  " << member.service_name << "  port " << member.port
              << "  unit " << member.unit_path.string() << "\n";
  }
  return 0;
}

int services_command(const bool json) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<std::vector<berth::ServiceRecord>> listed =
      berth::registry_list(paths.value());
  if (listed.is_err())
    return fail(listed.error().ctx());
  if (json) {
    std::cout << berth::json_text(berth::services_json(listed.value()));
    return 0;
  }
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

// add/remove BLOCK (download + systemctl): never run them under the TUI.
namespace {

void print_proxy_usage() {
  std::cerr << "usage: berthpanel proxy add <service> <domain>\n"
               "       berthpanel proxy list\n"
               "       berthpanel proxy remove <domain>\n";
}

int proxy_add_command(const berth::Paths& paths, const char* service,
                      const char* domain) {
  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());
  const berth::Result<void> added = berth::proxy_add(paths, service, domain,
                                                     &cancel_flag);
  if (added.is_err())
    return fail(added.error().ctx());
  std::cout << "routed " << berth::normalize_proxy_domain(domain) << " -> "
            << service << "\n";
  return 0;
}

int proxy_remove_command(const berth::Paths& paths, const char* domain) {
  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());
  const berth::Result<bool> removed =
      berth::proxy_remove(paths, domain, &cancel_flag);
  if (removed.is_err())
    return fail(removed.error().ctx());
  std::cout << (removed.value() ? "removed " : "no route for ")
            << berth::normalize_proxy_domain(domain) << "\n";
  return removed.value() ? 0 : 1;
}

}

int proxy_command(const std::span<const char* const>& args) {
  if (args.size() < 2) {
    print_proxy_usage();
    return 2;
  }
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const std::string_view sub(args[1]);

  if (sub == "list") {
    const berth::Result<std::vector<berth::ProxyRoute>> routes =
        berth::proxy_routes_load(paths.value());
    if (routes.is_err())
      return fail(routes.error().ctx());
    if (routes.value().empty()) {
      std::cout << "no proxy routes\n";
      return 0;
    }
    for (const berth::ProxyRoute& route : routes.value()) {
      std::cout << "  " << route.domain << " -> " << route.service_name
                << " 127.0.0.1:" << route.port << "\n";
    }
    return 0;
  }
  if (sub == "add" && args.size() == 4)
    return proxy_add_command(paths.value(), args[2], args[3]);
  if (sub == "remove" && args.size() == 3)
    return proxy_remove_command(paths.value(), args[2]);

  print_proxy_usage();
  return 2;
}

// The service must be stopped.
int backup_command(const std::string_view service,
                   const std::string_view archive_arg) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const std::filesystem::path output =
      archive_arg.empty() ? berth::default_backup_path(paths.value(), service)
                          : std::filesystem::path(archive_arg);

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  const berth::Result<berth::BackupReport> report =
      berth::backup_service(paths.value(), service, output, &cancel_flag);
  if (report.is_err())
    return fail(report.error().ctx());
  std::cout << "backed up " << service << " -> " << output.string() << "\n"
            << "  files: " << report.value().files
            << "  dirs: " << report.value().directories
            << "  bytes: " << report.value().bytes
            << (report.value().had_secrets ? "  (includes secrets)" : "")
            << "\n";
  return 0;
}

// The service must be stopped.
int restore_command(const std::string_view service,
                    const std::string_view archive_arg, const bool force) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  const berth::Result<berth::RestoreReport> report = berth::restore_service(
      paths.value(), service, std::filesystem::path(archive_arg), force,
      &cancel_flag);
  if (report.is_err())
    return fail(report.error().ctx());
  std::cout << "restored " << service << " from " << archive_arg << "\n"
            << "  data: " << (report.value().restored_data ? "yes" : "no")
            << "  secrets: " << (report.value().restored_secrets ? "yes" : "no")
            << "  files: " << report.value().files << "\n";
  return 0;
}

int verify_sandbox_command() {
  const berth::Result<berth::SandboxAudit> audit = berth::verify_sandbox();
  if (audit.is_err())
    return fail(audit.error().ctx());
  std::cout << berth::render_sandbox_audit(audit.value());
  return audit.value().full_grade() ? 0 : 1;
}

// Live E2E against the real systemd user manager; no network, no writes.
int selftest_command() {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  char self[4096];
  const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0)
    return fail("--selftest: cannot resolve /proc/self/exe");
  self[n] = '\0';

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  const berth::Result<berth::SelftestReport> report = berth::run_selftest(
      paths.value(), std::filesystem::path(self), &cancel_flag);
  if (report.is_err())
    return fail(report.error().ctx());
  std::cout << berth::render_selftest_report(report.value());
  return report.value().ok() ? 0 : 1;
}

int recipe_list_command(const bool json) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<std::vector<berth::Recipe>> recipes =
      berth::user_recipes_load(paths.value());
  if (recipes.is_err())
    return fail(recipes.error().ctx());
  if (json) {
    std::cout << berth::json_text(berth::user_recipes_json(recipes.value()));
    return 0;
  }
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

// Fixes an EXISTING user recipe, validates and stores it, but never deploys.
int recipe_edit_command(const std::string_view service,
                        const std::span<const char* const>& args) {
  const std::vector<std::string> tokens(args.begin(), args.end());
  const berth::Result<berth::RecipeEdits> edits =
      berth::parse_recipe_edit_args(tokens);
  if (edits.is_err())
    return fail(edits.error().ctx());

  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  const berth::Result<berth::RecipeEditResult> updated =
      berth::user_recipe_update(paths.value(), service, edits.value());
  if (updated.is_err())
    return fail(updated.error().ctx());

  std::cout << "updated " << updated.value().after.service_name << "\n"
            << berth::render_recipe_edit_diff(updated.value().before,
                                              updated.value().after)
            << "  stored: " << updated.value().stored_path.string() << "\n";
  return 0;
}

int secret_set_command(const std::string_view service,
                       const std::string_view key) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  // The value comes from stdin, never argv: argv is world-readable in ps.
  std::string value;
  if (!std::getline(std::cin, value)) {
    std::cerr << "berthpanel: __secret-set: no value on stdin\n";
    return 2;
  }
  const berth::Result<void> rotated = berth::rotate_secret(
      paths.value(), std::string(service), std::string(key), value,
      [](const std::string& name) { return berth::service_restart(name); });
  if (rotated.is_err())
    return fail(rotated.error().ctx());
  std::cout << "rotated " << key << " for " << service << "; restarted "
            << service << "\n";
  return 0;
}

int secret_keys_command(const std::string_view service) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<std::vector<std::string>> keys =
      berth::list_secret_keys(paths.value(), std::string(service));
  if (keys.is_err())
    return fail(keys.error().ctx());
  for (const std::string& key : keys.value())
    std::cout << key << "\n";
  return 0;
}

int repair_command(const bool json) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());
  const berth::Result<berth::RepairSnapshot> snapshot =
      berth::collect_repair_snapshot(paths.value(),
                                     berth::systemd_unit_probe());
  if (snapshot.is_err())
    return fail(snapshot.error().ctx());
  const berth::RepairReport report = berth::repair_check(snapshot.value());
  if (json)
    std::cout << berth::json_text(berth::repair_json(report));
  else
    std::cout << berth::render_repair_report(report);
  return report.findings.empty() ? 0 : 1;
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

  // Partial success still counts; the report names what stayed behind.
  return report.value().failed.empty() ? 0 : 1;
}

int serve_static_command(const std::string_view root_arg,
                         const std::string_view listen_arg) {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  if (!listen_arg.empty()) {
    const berth::Result<std::pair<std::string, std::uint16_t>> parsed =
        berth::parse_listen_arg(listen_arg);
    if (parsed.is_err())
      return fail(parsed.error().ctx());
    address = parsed.value().first;
    port = parsed.value().second;
  }
  const berth::Result<berth::RequestHandler> handler =
      berth::make_static_handler(std::filesystem::path(root_arg));
  if (handler.is_err())
    return fail(handler.error().ctx());

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  berth::ServerConfig config;
  config.listen_address = address;
  config.port = port;
  const std::string host = address == "::1" ? "[::1]" : address;
  const berth::Result<void> ran = berth::run_http_server(
      config, handler.value(), &cancel_flag,
      [&host](const std::uint16_t bound) {
        std::cout << "listening on http://" << host << ":" << bound << "\n"
                  << std::flush;
      });
  if (ran.is_err())
    return fail(ran.error().ctx());
  std::cout << "stopped\n";
  return 0;
}

namespace {

void note_error(berth::StatusSnapshot& snapshot, const std::string& what) {
  if (!snapshot.error.empty())
    snapshot.error += "; ";
  snapshot.error += what;
}

// The status page reports what the registry, the port scan and the journal
// know; each failure joins the error line instead of hiding the others.
berth::StatusSnapshot collect_status_snapshot(const berth::Paths& paths) {
  berth::StatusSnapshot snapshot;
  const berth::Result<std::vector<berth::ServiceRecord>> listed =
      berth::registry_list(paths);
  if (listed.is_err()) {
    note_error(snapshot, "registry: " + listed.error().ctx());
  } else {
    for (const berth::ServiceRecord& record : listed.value()) {
      snapshot.services.push_back(berth::StatusService{
          record.service_name, record.display_name, record.description,
          record.repo, record.commit, record.port});
    }
  }
  const berth::Result<berth::PortReport> ports = berth::listening_ports(paths);
  if (ports.is_err()) {
    note_error(snapshot, "ports: " + ports.error().ctx());
  } else {
    for (const berth::ListeningPort& p : ports.value().ports) {
      snapshot.ports.push_back(
          berth::StatusPort{p.address, p.port, p.loopback, p.wildcard, p.owned});
    }
  }
  const berth::Result<std::vector<berth::JournalEntry>> journal =
      berth::journal_load(paths);
  if (journal.is_err()) {
    note_error(snapshot, "journal: " + journal.error().ctx());
  } else {
    for (const berth::JournalEntry& entry : journal.value()) {
      snapshot.open_deploys.push_back(berth::StatusDeploy{
          entry.service_name, entry.stage, entry.started_at_unix});
    }
  }
  return snapshot;
}

}

int serve_status_command(const std::string_view listen_arg) {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  if (!listen_arg.empty()) {
    const berth::Result<std::pair<std::string, std::uint16_t>> parsed =
        berth::parse_listen_arg(listen_arg);
    if (parsed.is_err())
      return fail(parsed.error().ctx());
    address = parsed.value().first;
    port = parsed.value().second;
  }
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  const berth::Paths captured = paths.value();
  berth::RequestHandler handler = berth::make_status_handler(
      [captured]() { return collect_status_snapshot(captured); });

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  berth::ServerConfig config;
  config.listen_address = address;
  config.port = port;
  const std::string host = address == "::1" ? "[::1]" : address;
  const berth::Result<void> ran = berth::run_http_server(
      config, handler, &cancel_flag, [&host](const std::uint16_t bound) {
        std::cout << "listening on http://" << host << ":" << bound << "\n"
                  << std::flush;
      });
  if (ran.is_err())
    return fail(ran.error().ctx());
  std::cout << "stopped\n";
  return 0;
}

// The clone+detect FLOW lives in berth/wizard.cpp; a second copy would drift.
int recipe_draft_command(const std::string_view url_arg,
                         const std::string_view ref_arg, const bool apply,
                         const bool force) {
  const berth::Result<berth::Paths> paths = berth::paths_from_env();
  if (paths.is_err())
    return fail(paths.error().ctx());

  std::atomic<bool> cancel_flag{false};
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&cancel_flag);
  if (signalling.is_err())
    return fail(signalling.error().ctx());

  const berth::Result<berth::DraftResult> prepared = berth::prepare_draft(
      paths.value(),
      berth::DraftRequest{.url = std::string(url_arg),
                          .ref = std::string(ref_arg)},
      &cancel_flag, [](const std::string& line) { std::cout << line << "\n"; });
  if (prepared.is_err())
    return fail(prepared.error().ctx());
  const berth::DraftResult& result = prepared.value();

  // Asked before storing: an overwrite must be an explicit act (§8).
  const berth::Result<berth::DraftClash> clash =
      berth::find_recipe_clash(paths.value(), result.draft.service_name);
  if (clash.is_err())
    return fail(clash.error().ctx());
  if (clash.value().exists && !force) {
    std::cerr << "refusing to overwrite: " << clash.value().service_name
              << (clash.value().in_catalog ? " is in the builtin catalog"
                                           : " is already stored")
              << "\n"
              << "  existing: "
              << (clash.value().existing_commit.empty()
                      ? std::string("(unpinned)")
                      : clash.value().existing_commit)
              << "\n  draft:    " << result.commit << "\n"
              << "rerun with --force to store the draft over it\n";
    return 2;
  }

  // The draft is ALWAYS saved: what is deployable is what is on disk.
  const berth::Result<std::filesystem::path> stored =
      berth::store_draft(paths.value(), result.draft, clash.value().exists);
  if (stored.is_err())
    return fail(stored.error().ctx());

  std::cout << berth::render_draft_report(result, clash.value())
            << "  saved: " << stored.value().string() << "\n"
            << "the heuristics may be wrong — edit " << stored.value().string()
            << ", then:\n"
            << "  berthpanel __recipe-add " << stored.value().string()
            << " --force\n";
  if (apply) {
    std::cout << "deploy it with:\n"
              << "  berthpanel __deploy " << result.draft.service_name << "\n";
  }
  return 0;
}

namespace {

std::string_view listen_arg(const std::span<const char* const>& args,
                            std::size_t from) {
  std::string_view listen;
  for (std::size_t i = from; i < args.size(); ++i) {
    const std::string_view arg(args[i]);
    if (arg == "--listen" && i + 1 < args.size()) {
      listen = args[i + 1];
      ++i;
    } else if (arg.rfind("--listen=", 0) == 0) {
      listen = arg.substr(9);
    }
  }
  return listen;
}

int deploy_verb(const std::span<const char* const>& args) {
  std::string_view port_arg;
  bool upgrade = false;
  bool json = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string_view arg(args[i]);
    if (arg == "--upgrade") {
      upgrade = true;
    } else if (arg == "--json") {
      json = true;
    } else if (port_arg.empty()) {
      port_arg = args[i];
    }
  }
  return deploy_command(args[1], port_arg, upgrade, json);
}

int recipe_draft_verb(const std::span<const char* const>& args) {
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

int restore_verb(const std::span<const char* const>& args) {
  bool force = false;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (std::string_view(args[i]) == "--force")
      force = true;
  }
  return restore_command(args[1], args[2], force);
}

// The __ verbs are the contract with the units and the TUI, not with a human.
std::optional<int> dispatch_verb(const std::span<const char* const>& args) {
  const std::string_view first(args[0]);
  if (first == "proxy")
    return proxy_command(args);
  if (first == "backup" && args.size() >= 2 && args.size() <= 3) {
    return backup_command(args[1], args.size() == 3 ? args[2] : "");
  }
  if (first == "restore" && args.size() >= 3)
    return restore_verb(args);
  if (first == "__egress-probe")
    return egress_probe();
  if (first == "__catalog") {
    if (contains_flag(args, "--json"))
      std::cout << berth::json_text(berth::catalog_json());
    else
      std::cout << berth::render_catalog_list();
    return 0;
  }
  if (first == "__services")
    return services_command(contains_flag(args, "--json"));
  if (first == "__recipe-list")
    return recipe_list_command(contains_flag(args, "--json"));
  if (first == "__recipe-add" && args.size() == 2)
    return recipe_add_command(args[1], false);
  if (first == "__recipe-add" && args.size() == 3 &&
      std::string_view(args[2]) == "--force") {
    return recipe_add_command(args[1], true);
  }
  if (first == "__recipe-remove" && args.size() == 2)
    return recipe_remove_command(args[1]);
  if (first == "__recipe-edit" && args.size() >= 2)
    return recipe_edit_command(args[1], args.subspan(2));
  if (first == "__recipe-draft" && args.size() >= 2)
    return recipe_draft_verb(args);
  if (first == "__secret-set" && args.size() == 3)
    return secret_set_command(args[1], args[2]);
  if (first == "__secret-keys" && args.size() == 2)
    return secret_keys_command(args[1]);
  if (first == "__deploy" && args.size() >= 2)
    return deploy_verb(args);
  if (first == "__stacks")
    return stacks_command(contains_flag(args, "--json"));
  if (first == "__deploy-stack" && args.size() >= 2)
    return deploy_stack_command(args[1], contains_flag(args, "--json"));
  if (first == "__cleanup") {
    return cleanup_command(contains_flag(args, "--install"),
                           contains_flag(args, "--quiet"));
  }
  if (first == "__repair")
    return repair_command(contains_flag(args, "--json"));
  if (first == "__serve-static" && args.size() >= 2)
    return serve_static_command(args[1], listen_arg(args, 2));
  if (first == "__serve-status")
    return serve_status_command(listen_arg(args, 1));
  // Test hook for the SIGKILL flock scenario (stage 4.3), not a user command.
  if (first == "__test-lock-child" && args.size() == 2)
    return berth::lock_test_child(args[1]);
  return std::nullopt;
}

}

int run(const std::span<const char* const> args) {
  if (args.empty()) {
    // A pipe or a file gets the usage text, never a full-screen UI.
    if (!::isatty(STDOUT_FILENO)) {
      print_usage();
      return 2;
    }
    const berth::Result<berth::Paths> paths = berth::paths_from_env();
    if (paths.is_err())
      return fail(paths.error().ctx());
    return tui::run_tui(paths.value());
  }
  if (const std::optional<int> handled = dispatch_verb(args))
    return *handled;
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
    if (a == "--selftest")
      return selftest_command();
  }
  print_usage();
  return 2;
}

}
int main(int argc, char* argv[]) {
  // Without this, an early-closed pipe kills the panel.
  ::signal(SIGPIPE, SIG_IGN);

  const char* const* first = argc > 0 ? argv + 1 : nullptr;
  const std::span<const char* const> args(
      first, argc > 0 ? static_cast<std::size_t>(argc - 1) : std::size_t{0});
  return run(args);
}
