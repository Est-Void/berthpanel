#pragma once

// src/berth/proxy.h — the managed Caddy reverse proxy (stage 5B.2): a
// prebuilt Caddy installed through toolbox, a rendered Caddyfile, and the
// systemd user unit that runs it. The Caddyfile is GENERATED from a small
// route store, never hand-edited: a template with one validated site block
// per route, so a hostile domain cannot inject a directive (§8/§11).
//
// EGRESS BOUNDARY — read this before adding anything (dev.md §6.1, §8):
//   * Caddy's own outbound traffic (Let's Encrypt ACME, OCSP) is the RUNTIME
//     egress of a deployed service, exactly like Uptime Kuma or Vaultwarden.
//     It does NOT go through httpclient, does NOT pass kAllowedEndpoints, and
//     does NOT appear in net.log. The panel does not proxy it.
//   * The ONLY request the PANEL makes for Caddy is the tool download in
//     ensure_caddy_installed(), which goes through toolbox/httpclient and is
//     therefore allowlisted (github.com, release-assets.githubusercontent.com)
//     and net.log-logged like every other panel request.
//   * If ACME ever moves into the panel's httpclient, it needs the §6.1
//     procedure: a kAllowedEndpoints entry plus its test in one commit.
//
// Low ports (dev.md §6.8, stage 5B.1): Caddy must bind 80/443, which an
// unprivileged user unit can only do after the host opted into
// ip_unprivileged_port_start=0 or a forward. This module renders and starts
// the unit either way; a bind failure is systemd's loud error, not a silent
// fallback to a high port.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/paths.h"
#include "units.h"
#include "util/result.h"

namespace berth {

inline constexpr std::string_view kCaddyUnitStem = "berthpanel-caddy";

// One reverse-proxy route: a public domain fronted to one registered
// service's loopback port.
struct ProxyRoute {
  std::string domain;        // lowercased, validated hostname
  std::string service_name;  // registry key (the berthpanel- unit stem)
  std::uint16_t port = 0;    // the service's loopback port
};

// Strict hostname validation, fail loudly. Accepts ASCII DNS names (and
// punycode labels, "xn--..."): at least two labels, each 1..63 chars of
// [a-z0-9-] not starting/ending with '-', total <= 253, last label not all
// digits (so a bare IPv4 is refused). Rejects a trailing dot, whitespace,
// control characters, '{' '}' and any non-ASCII byte: an IDN must arrive as
// its punycode form, because a raw U-label in a Caddyfile is an injection
// risk and ACME would refuse it anyway.
[[nodiscard]] Result<void> validate_proxy_domain(std::string_view domain);

// Lowercases a domain for storage. Callers validate the result.
[[nodiscard]] std::string normalize_proxy_domain(std::string_view domain);

// The reverse_proxy target line for one upstream. Pure, and refuses any host
// but 127.0.0.1: a route that could point at another interface would turn the
// panel into a way to expose a service the recipe deliberately bound to
// loopback (§8 loopback-only contract).
[[nodiscard]] Result<std::string> render_reverse_proxy_line(
    std::string_view upstream_host, std::uint16_t port);

// One Caddyfile site block: the validated domain, its reverse_proxy line, and
// a brace block that a domain containing '{' or '}' cannot escape (the
// validator already refuses those).
[[nodiscard]] Result<std::string> render_route_block(const ProxyRoute& route);

// The WHOLE Caddyfile: a global block (admin off — no local config API, and
// cert storage pinned to the service data dir so ProtectHome=read-only does
// not break ACME) plus one site block per route, sorted by domain. A
// duplicate domain is a loud Err.
[[nodiscard]] Result<std::string> render_caddyfile(
    const std::vector<ProxyRoute>& routes,
    const std::filesystem::path& caddy_data_dir);

// The route store (state_dir/proxy-routes.json), keyed by domain.
[[nodiscard]] Result<std::vector<ProxyRoute>> proxy_routes_load(
    const Paths& paths);
[[nodiscard]] Result<void> proxy_routes_store(
    const Paths& paths, const std::vector<ProxyRoute>& routes);

// PURE merge used by `proxy add`: same domain + same service updates the
// port (idempotent), same domain + a different service is a loud Err, a new
// domain appends. Inputs are assumed already validated/normalized.
[[nodiscard]] Result<std::vector<ProxyRoute>> upsert_proxy_route(
    const std::vector<ProxyRoute>& existing, std::string_view service,
    std::string_view domain, std::uint16_t port);

// PURE drop used by `proxy remove`. Removing an absent domain is not an
// error; it just returns the input unchanged.
[[nodiscard]] std::vector<ProxyRoute> remove_proxy_route(
    const std::vector<ProxyRoute>& existing, std::string_view domain);

// Caddy's fixed locations. The data dir holds certs and ACME state and must
// survive a reinstall; the config dir holds the generated Caddyfile.
[[nodiscard]] std::filesystem::path caddy_binary_path(const Paths& paths);
[[nodiscard]] std::filesystem::path caddy_data_dir(const Paths& paths);
[[nodiscard]] std::filesystem::path caddy_config_dir(const Paths& paths);
[[nodiscard]] std::filesystem::path caddyfile_path(const Paths& paths);

// The managed unit as a UnitSpec (rendered/written through units.cpp, so it
// gets the same hardening and mandatory limits as every service).
[[nodiscard]] Result<UnitSpec> caddy_unit_spec(const Paths& paths);

// True when the pinned Caddy binary is present and executable.
[[nodiscard]] bool caddy_installed(const Paths& paths);

// Downloads and verifies the pinned Caddy via toolbox. The URL is
// allowlisted and net.log-logged; a hash mismatch is a loud failure.
[[nodiscard]] Result<void> ensure_caddy_installed(const Paths& paths,
                                                  std::atomic<bool>* cancel);

// Regenerates the Caddyfile from the store, writes the unit, daemon-reloads
// and starts or restarts Caddy. BLOCKING (systemctl): never call it on the
// TUI render thread.
[[nodiscard]] Result<void> proxy_sync(const Paths& paths,
                                      std::atomic<bool>* cancel);

// `proxy add <service> <domain>`: service must be registered, domain must be
// free (a domain owned by another service is an Err), then sync. Idempotent:
// re-adding the same service/domain is an update, not a duplicate.
[[nodiscard]] Result<void> proxy_add(const Paths& paths,
                                     std::string_view service,
                                     std::string_view domain,
                                     std::atomic<bool>* cancel);

// `proxy remove <domain>`: drops the route (idempotent) and syncs. Removing
// the last route leaves the unit with an empty Caddyfile; it is not
// auto-stopped, so the operator decides when Caddy goes away.
[[nodiscard]] Result<bool> proxy_remove(const Paths& paths,
                                        std::string_view domain,
                                        std::atomic<bool>* cancel);

}  // namespace berth
