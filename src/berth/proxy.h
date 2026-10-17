#pragma once

// The Caddyfile is GENERATED from the route store, never hand-edited: one
// validated site block per route, so a domain cannot inject a directive (§11).
// EGRESS BOUNDARY: Caddy's own traffic (ACME, OCSP) is the RUNTIME egress of a
// deployed service — not through kAllowedEndpoints, not net.log, not proxied by
// the panel; only the tool download is panel egress (§6.1). Low ports (§6.8):
// a bind failure is systemd's loud error, never a silent fallback.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/paths.h"
#include "units.h"
#include "util/result.h"

namespace berth {

inline constexpr std::string_view kCaddyUnitStem = "berthpanel-caddy";

struct ProxyRoute {
  std::string domain;
  std::string service_name;
  std::uint16_t port = 0;
};

// ASCII DNS name (punycode labels included): >= 2 labels, each 1..63 of
// [a-z0-9-] with no edge dash, total <= 253, last label not all digits. A
// trailing dot, whitespace, control bytes, '{' '}' and non-ASCII are refused.
[[nodiscard]] Result<void> validate_proxy_domain(std::string_view domain);

[[nodiscard]] std::string normalize_proxy_domain(std::string_view domain);

// Only 127.0.0.1: another interface would expose a service bound to loopback.
[[nodiscard]] Result<std::string> render_reverse_proxy_line(
    std::string_view upstream_host, std::uint16_t port);

[[nodiscard]] Result<std::string> render_route_block(const ProxyRoute& route);

[[nodiscard]] Result<std::string> render_caddyfile(
    const std::vector<ProxyRoute>& routes,
    const std::filesystem::path& caddy_data_dir);

[[nodiscard]] Result<std::vector<ProxyRoute>> proxy_routes_load(
    const Paths& paths);
[[nodiscard]] Result<void> proxy_routes_store(
    const Paths& paths, const std::vector<ProxyRoute>& routes);

// Sorted by domain; the versioned envelope for `proxy list --json`.
[[nodiscard]] nlohmann::json proxy_routes_json(
    const std::vector<ProxyRoute>& routes);

// Same domain + same service updates the port; another service is a loud Err.
[[nodiscard]] Result<std::vector<ProxyRoute>> upsert_proxy_route(
    const std::vector<ProxyRoute>& existing, std::string_view service,
    std::string_view domain, std::uint16_t port);

[[nodiscard]] std::vector<ProxyRoute> remove_proxy_route(
    const std::vector<ProxyRoute>& existing, std::string_view domain);

[[nodiscard]] std::filesystem::path caddy_binary_path(const Paths& paths);
[[nodiscard]] std::filesystem::path caddy_data_dir(const Paths& paths);
[[nodiscard]] std::filesystem::path caddy_config_dir(const Paths& paths);
[[nodiscard]] std::filesystem::path caddyfile_path(const Paths& paths);

[[nodiscard]] Result<UnitSpec> caddy_unit_spec(const Paths& paths);

[[nodiscard]] bool caddy_installed(const Paths& paths);

// Allowlisted and net.log-logged; a hash mismatch is a loud failure.
[[nodiscard]] Result<void> ensure_caddy_installed(const Paths& paths,
                                                  std::atomic<bool>* cancel);

// The systemd/toolbox seam. Injected so tests can pin the stop/restart policy
// without a live user manager or a download.
class ProxyOps {
 public:
  virtual ~ProxyOps() = default;
  virtual Result<void> install_caddy(const Paths& paths,
                                     std::atomic<bool>* cancel) = 0;
  virtual Result<void> daemon_reload() = 0;
  virtual Result<int> is_active(const std::string& unit) = 0;
  virtual Result<void> run(const std::vector<std::string>& argv) = 0;
};

// BLOCKING (systemctl): never call on the TUI render thread.
[[nodiscard]] Result<void> proxy_sync(const Paths& paths,
                                      std::atomic<bool>* cancel);

[[nodiscard]] Result<void> proxy_sync_with(const Paths& paths,
                                           std::atomic<bool>* cancel,
                                           ProxyOps& ops);

[[nodiscard]] Result<void> proxy_add(const Paths& paths,
                                     std::string_view service,
                                     std::string_view domain,
                                     std::atomic<bool>* cancel);

// The last route leaves an empty Caddyfile AND stops+disables Caddy: no sites
// means nothing should keep running (stage 10.5, debt 5B).
[[nodiscard]] Result<bool> proxy_remove(const Paths& paths,
                                        std::string_view domain,
                                        std::atomic<bool>* cancel);

}
