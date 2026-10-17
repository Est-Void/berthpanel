#include "proxy.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "core/atomic.h"
#include "core/docstore.h"
#include "core/jsonout.h"
#include "core/names.h"
#include "core/state.h"
#include "toolbox.h"
#include "util/fs.h"
#include "util/strutil.h"

namespace berth {

namespace {

// Under github.com, so allowlisted; toolbox follows the allowlisted redirect.
constexpr std::string_view kCaddyVersion = "2.11.7";
constexpr std::string_view kCaddyAmd64Url =
    "https://github.com/caddyserver/caddy/releases/download/v2.11.7/"
    "caddy_2.11.7_linux_amd64.tar.gz";
constexpr std::string_view kCaddyAmd64Sha =
    "727b91701a392de6ebc5027509f548bf39979e5216340d0faed8fa5e69c84f8b";
constexpr std::string_view kCaddyArm64Url =
    "https://github.com/caddyserver/caddy/releases/download/v2.11.7/"
    "caddy_2.11.7_linux_arm64.tar.gz";
constexpr std::string_view kCaddyArm64Sha =
    "d8fc6d179a5d283028a472a5618564f6ad8a86fed513e64f032b3b0b7cc45e42";

constexpr std::string_view kCaddyMember = "caddy";

bool is_alpha(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(const char c) {
  return c >= '0' && c <= '9';
}

bool is_label_char(const char c) {
  return is_alpha(c) || is_digit(c) || c == '-';
}

Result<std::string> caddyfile_quote(const std::string_view value) {
  for (const char c : value) {
    if (c == '\n' || c == '\r' || c == '\0') {
      return Error("proxy: value contains a newline or NUL");
    }
  }
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  out += "\"";
  return out;
}

Result<PinnedTool> caddy_tool(const Paths& paths) {
#if defined(__x86_64__)
  const std::string url(kCaddyAmd64Url);
  const std::string sha(kCaddyAmd64Sha);
#elif defined(__aarch64__)
  const std::string url(kCaddyArm64Url);
  const std::string sha(kCaddyArm64Sha);
#else
  return Error(
      "proxy: no pinned Caddy for this architecture (linux/amd64 and "
      "linux/arm64 only)");
#endif
  PinnedTool tool;
  tool.name = "caddy";
  tool.url = url;
  tool.sha256 = sha;
  tool.inner_path = std::string(kCaddyMember);
  tool.install_dir = paths.state_dir / "caddy";
  tool.executable = true;
  tool.archive = ToolArchive::kTarGz;
  return tool;
}

nlohmann::json encode_route(const ProxyRoute& route) {
  return nlohmann::json{{"domain", route.domain},
                        {"service_name", route.service_name},
                        {"port", route.port}};
}

ProxyRoute decode_route(const nlohmann::json& doc) {
  ProxyRoute route;
  route.domain = doc.at("domain").get<std::string>();
  route.service_name = doc.at("service_name").get<std::string>();
  const int port = doc.at("port").get<int>();
  if (port < 1 || port > 65535) {
    throw nlohmann::json::out_of_range::create(406, "port out of range",
                                               nullptr);
  }
  route.port = static_cast<std::uint16_t>(port);
  // A hand-edited store must not smuggle an invalid name or domain into a unit
  // path or a Caddyfile (§11).
  if (!validate_unit_name(route.service_name)) {
    throw nlohmann::json::type_error::create(
        302, "invalid service_name '" + route.service_name + "'", nullptr);
  }
  if (validate_proxy_domain(route.domain).is_err()) {
    throw nlohmann::json::type_error::create(
        302, "invalid domain '" + route.domain + "'", nullptr);
  }
  return route;
}

DocStore<ProxyRoute> route_store(const Paths& paths) {
  return DocStore<ProxyRoute>(
      paths.state_dir / "proxy-routes.json", paths.state_dir, 1, "routes",
      encode_route, decode_route,
      [](const ProxyRoute& route) { return route.domain; });
}

Result<void> write_caddyfile(const Paths& paths,
                             const std::vector<ProxyRoute>& routes) {
  Result<std::string> text = render_caddyfile(routes, caddy_data_dir(paths));
  if (text.is_err())
    return std::move(text).error();
  Result<void> config = ensure_dir(caddy_config_dir(paths));
  if (config.is_err())
    return std::move(config).error();
  Result<void> data = ensure_dir(caddy_data_dir(paths));
  if (data.is_err())
    return std::move(data).error();
  // Domains and loopback ports only, so 0644.
  Result<void> written =
      atomic_write(caddyfile_path(paths), text.value(), kPermShared);
  if (written.is_err())
    return std::move(written).error();
  return {};
}

Result<void> validate_domain_chars(const std::string_view domain) {
  for (const char raw : domain) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c >= 0x80) {
      return Error("proxy: domain '" + std::string(domain) +
                   "' is not ASCII; use its punycode (xn--) form");
    }
    if (c <= 0x20 || c == 0x7f) {
      return Error("proxy: domain '" + std::string(domain) +
                   "' contains whitespace or a control character");
    }
    if (!is_label_char(raw) && raw != '.') {
      return Error("proxy: domain '" + std::string(domain) +
                   "' has an invalid character '" + std::string(1, raw) + "'");
    }
  }
  return {};
}

Result<void> validate_domain_labels(const std::string_view domain) {
  std::size_t labels = 0;
  std::string_view last;
  std::size_t start = 0;
  while (start <= domain.size()) {
    const std::size_t dot = domain.find('.', start);
    const std::string_view label = domain.substr(
        start,
        dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (label.empty())
      return Error("proxy: domain '" + std::string(domain) +
                   "' has an empty label");
    if (label.size() > 63)
      return Error("proxy: domain label is longer than 63 characters");
    if (label.front() == '-' || label.back() == '-')
      return Error("proxy: domain label may not start or end with '-'");
    ++labels;
    last = label;
    if (dot == std::string_view::npos)
      break;
    start = dot + 1;
  }
  if (labels < 2) {
    return Error("proxy: domain '" + std::string(domain) +
                 "' needs at least one dot (a bare hostname is not a public "
                 "TLS name)");
  }
  if (last.size() < 2)
    return Error("proxy: domain '" + std::string(domain) +
                 "' has a too-short final label");
  if (std::all_of(last.begin(), last.end(), is_digit)) {
    return Error("proxy: domain '" + std::string(domain) +
                 "' looks like an IP address, not a name");
  }
  return {};
}

}
Result<void> validate_proxy_domain(const std::string_view domain) {
  if (domain.empty())
    return Error("proxy: domain is empty");
  if (domain.size() > 253)
    return Error("proxy: domain is longer than 253 characters");
  if (domain.back() == '.')
    return Error("proxy: domain must not end with a dot: '" +
                 std::string(domain) + "'");
  const Result<void> chars = validate_domain_chars(domain);
  if (chars.is_err())
    return std::move(chars).error();
  return validate_domain_labels(domain);
}

std::string normalize_proxy_domain(const std::string_view domain) {
  std::string out(domain);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

Result<std::string> render_reverse_proxy_line(
    const std::string_view upstream_host, const std::uint16_t port) {
  if (upstream_host != "127.0.0.1") {
    return Error("proxy: upstream must be 127.0.0.1, not '" +
                 std::string(upstream_host) + "'");
  }
  if (port == 0)
    return Error("proxy: upstream port is not set");
  return "reverse_proxy 127.0.0.1:" + std::to_string(port);
}

Result<std::string> render_route_block(const ProxyRoute& route) {
  const Result<void> valid = validate_proxy_domain(route.domain);
  if (valid.is_err())
    return std::move(valid).error();
  Result<std::string> line = render_reverse_proxy_line("127.0.0.1", route.port);
  if (line.is_err())
    return std::move(line).error();
  return route.domain + " {\n\t" + line.value() + "\n}\n";
}

Result<std::string> render_caddyfile(const std::vector<ProxyRoute>& routes,
                                     const std::filesystem::path& data_dir) {
  if (data_dir.empty() || !data_dir.is_absolute()) {
    return Error("proxy: Caddy data dir must be an absolute path");
  }
  Result<std::string> storage = caddyfile_quote(data_dir.string());
  if (storage.is_err())
    return std::move(storage).error();

  // admin off: no unauthenticated localhost config API. Cert storage pinned so
  // ProtectHome=read-only cannot break ACME.
  std::string out =
      "{\n\tadmin off\n\tstorage file_system " + storage.value() + "\n}\n";

  std::vector<ProxyRoute> sorted = routes;
  std::sort(sorted.begin(), sorted.end(),
            [](const ProxyRoute& a, const ProxyRoute& b) {
              return a.domain < b.domain;
            });
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i > 0 && sorted[i].domain == sorted[i - 1].domain) {
      return Error("proxy: duplicate domain '" + sorted[i].domain + "'");
    }
    Result<std::string> block = render_route_block(sorted[i]);
    if (block.is_err())
      return std::move(block).error();
    out += "\n" + block.value();
  }
  return out;
}

Result<std::vector<ProxyRoute>> proxy_routes_load(const Paths& paths) {
  return route_store(paths).load();
}

Result<void> proxy_routes_store(const Paths& paths,
                                const std::vector<ProxyRoute>& routes) {
  return route_store(paths).store(routes);
}

nlohmann::json proxy_routes_json(const std::vector<ProxyRoute>& routes) {
  std::vector<ProxyRoute> sorted = routes;
  std::sort(sorted.begin(), sorted.end(),
            [](const ProxyRoute& a, const ProxyRoute& b) {
              return a.domain < b.domain;
            });
  nlohmann::json array = nlohmann::json::array();
  for (const ProxyRoute& route : sorted) {
    array.push_back({{"domain", route.domain},
                     {"service_name", route.service_name},
                     {"port", route.port}});
  }
  nlohmann::json doc = json_document("proxy-list");
  doc["routes"] = std::move(array);
  return doc;
}

Result<std::vector<ProxyRoute>> upsert_proxy_route(
    const std::vector<ProxyRoute>& existing, const std::string_view service,
    const std::string_view domain, const std::uint16_t port) {
  std::vector<ProxyRoute> routes = existing;
  for (ProxyRoute& route : routes) {
    if (route.domain != domain)
      continue;
    if (route.service_name != service) {
      return Error("proxy: domain '" + std::string(domain) +
                   "' is already routed to '" + route.service_name + "'");
    }
    route.port = port;
    return routes;
  }
  routes.push_back(ProxyRoute{.domain = std::string(domain),
                              .service_name = std::string(service),
                              .port = port});
  return routes;
}

std::vector<ProxyRoute> remove_proxy_route(
    const std::vector<ProxyRoute>& existing, const std::string_view domain) {
  std::vector<ProxyRoute> routes = existing;
  routes.erase(std::remove_if(routes.begin(), routes.end(),
                              [&](const ProxyRoute& route) {
                                return route.domain == domain;
                              }),
               routes.end());
  return routes;
}

std::filesystem::path caddy_binary_path(const Paths& paths) {
  return paths.state_dir / "caddy" / "caddy";
}

std::filesystem::path caddy_data_dir(const Paths& paths) {
  return paths.data_dir / "caddy";
}

std::filesystem::path caddy_config_dir(const Paths& paths) {
  return paths.config_dir / "caddy";
}

std::filesystem::path caddyfile_path(const Paths& paths) {
  return caddy_config_dir(paths) / "Caddyfile";
}

Result<UnitSpec> caddy_unit_spec(const Paths& paths) {
  UnitSpec spec;
  spec.service_name = std::string(kCaddyUnitStem);
  spec.description = "berthpanel managed Caddy reverse proxy";
  spec.working_dir = caddy_config_dir(paths);
  spec.exec_binary = caddy_binary_path(paths).string();
  spec.exec_args = {"run", "--config", caddyfile_path(paths).string(),
                    "--adapter", "caddyfile"};
  spec.limits = UnitLimits{.memory_max = "384M", .tasks_max = 512};
  // ProtectSystem=strict: Caddy still needs its cert store and config writable.
  spec.read_write_paths = {caddy_data_dir(paths), caddy_config_dir(paths)};
  return spec;
}

bool caddy_installed(const Paths& paths) {
  return ::access(caddy_binary_path(paths).c_str(), X_OK) == 0;
}

Result<void> ensure_caddy_installed(const Paths& paths,
                                    std::atomic<bool>* cancel) {
  if (caddy_installed(paths))
    return {};
  Result<PinnedTool> tool = caddy_tool(paths);
  if (tool.is_err())
    return std::move(tool).error();
  Result<std::filesystem::path> installed =
      install_pinned_tool(tool.value(), paths, cancel);
  if (installed.is_err()) {
    return with_context(
        std::move(installed).error(),
        "proxy: installing Caddy " + std::string(kCaddyVersion));
  }
  return {};
}

namespace {

class RealProxyOps final : public ProxyOps {
 public:
  explicit RealProxyOps(std::atomic<bool>* cancel) : cancel_(cancel) {}

  Result<void> install_caddy(const Paths& paths,
                             std::atomic<bool>* cancel) override {
    return ensure_caddy_installed(paths, cancel);
  }
  Result<void> daemon_reload() override { return systemctl_daemon_reload(); }
  Result<int> is_active(const std::string& unit) override {
    return systemctl_user_code({"is-active", unit},
                               std::chrono::milliseconds(20000), cancel_);
  }
  Result<void> run(const std::vector<std::string>& argv) override {
    return systemctl_user(argv);
  }

 private:
  std::atomic<bool>* cancel_;
};

}

Result<void> proxy_sync(const Paths& paths, std::atomic<bool>* cancel) {
  RealProxyOps ops(cancel);
  return proxy_sync_with(paths, cancel, ops);
}

Result<void> proxy_sync_with(const Paths& paths, std::atomic<bool>* cancel,
                             ProxyOps& ops) {
  Result<std::vector<ProxyRoute>> routes = proxy_routes_load(paths);
  if (routes.is_err())
    return std::move(routes).error();

  const Result<void> wrote = write_caddyfile(paths, routes.value());
  if (wrote.is_err())
    return std::move(wrote).error();

  const std::string unit = std::string(kCaddyUnitStem) + ".service";

  // Explicit policy (dev.md §8): admin off means there is no reload path. An
  // empty set stops+disables Caddy; a non-empty set restarts an active Caddy
  // or enables/starts an inactive one.
  if (routes.value().empty()) {
    const bool unit_present = std::filesystem::exists(paths.units_dir / unit);
    Result<int> active = ops.is_active(unit);
    if (active.is_err())
      return std::move(active).error();
    if (!unit_present && active.value() != 0)
      return {};  // nothing installed and nothing running: do not download
    return ops.run({"disable", "--now", unit});
  }

  const Result<void> installed = ops.install_caddy(paths, cancel);
  if (installed.is_err())
    return std::move(installed).error();

  Result<UnitSpec> spec = caddy_unit_spec(paths);
  if (spec.is_err())
    return std::move(spec).error();
  Result<std::filesystem::path> unit_path = write_unit(paths, spec.value());
  if (unit_path.is_err())
    return std::move(unit_path).error();

  const Result<void> reloaded = ops.daemon_reload();
  if (reloaded.is_err())
    return std::move(reloaded).error();

  Result<int> active = ops.is_active(unit);
  if (active.is_err())
    return std::move(active).error();

  Result<void> applied = active.value() == 0
                             ? ops.run({"restart", unit})
                             : ops.run({"enable", "--now", unit});
  if (applied.is_err()) {
    return with_context(
        std::move(applied).error(),
        "proxy: Caddy could not start. It must bind 80/443; on a default "
        "host an unprivileged user unit cannot, so either opt into "
        "net.ipv4.ip_unprivileged_port_start=0 (dev.md §6.8 / stage 5B.1) or "
        "keep Caddy on a high port and forward 80/443 to it");
  }
  return {};
}

Result<void> proxy_add(const Paths& paths, const std::string_view service,
                       const std::string_view domain,
                       std::atomic<bool>* cancel) {
  if (!validate_unit_name(service)) {
    return Error("proxy: invalid service name: '" + std::string(service) + "'");
  }
  const std::string normalized = normalize_proxy_domain(domain);
  const Result<void> valid = validate_proxy_domain(normalized);
  if (valid.is_err())
    return std::move(valid).error();

  const Result<std::optional<ServiceRecord>> record =
      registry_get(paths, service);
  if (record.is_err())
    return std::move(record).error();
  if (!record.value().has_value()) {
    return Error("proxy: no registered service named '" + std::string(service) +
                 "' — deploy it first");
  }
  if (record.value()->port == 0) {
    return Error("proxy: service '" + std::string(service) +
                 "' has no assigned loopback port");
  }

  Result<std::vector<ProxyRoute>> loaded = proxy_routes_load(paths);
  if (loaded.is_err())
    return std::move(loaded).error();

  Result<std::vector<ProxyRoute>> merged = upsert_proxy_route(
      loaded.value(), service, normalized, record.value()->port);
  if (merged.is_err())
    return std::move(merged).error();

  const Result<void> stored = proxy_routes_store(paths, merged.value());
  if (stored.is_err())
    return std::move(stored).error();

  return proxy_sync(paths, cancel);
}

Result<bool> proxy_remove(const Paths& paths, const std::string_view domain,
                          std::atomic<bool>* cancel) {
  const std::string normalized = normalize_proxy_domain(domain);
  const Result<void> valid = validate_proxy_domain(normalized);
  if (valid.is_err())
    return std::move(valid).error();

  Result<std::vector<ProxyRoute>> loaded = proxy_routes_load(paths);
  if (loaded.is_err())
    return std::move(loaded).error();

  std::vector<ProxyRoute> routes =
      remove_proxy_route(loaded.value(), normalized);
  if (routes.size() == loaded.value().size())
    return false;  // idempotent: absent is not an error

  const Result<void> stored = proxy_routes_store(paths, routes);
  if (stored.is_err())
    return std::move(stored).error();
  const Result<void> synced = proxy_sync(paths, cancel);
  if (synced.is_err())
    return std::move(synced).error();
  return true;
}

}
