// Tests for proxy.h (stage 5B.2): strict domain validation (a domain that
// could inject a Caddyfile directive must be refused), pure Caddyfile
// rendering, the loopback-only upstream rule, the route store, and the
// unknown-service refusal. Nothing here touches the network or systemd.

#include "berth/proxy.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::ProxyRoute;
using berth::render_caddyfile;
using berth::render_reverse_proxy_line;
using berth::render_route_block;
using berth::validate_proxy_domain;

namespace {

berth::Paths proxy_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.data_dir = root / "data" / "berthpanel";
  p.config_dir = root / "config" / "berthpanel";
  p.state_json = p.state_dir / "state.json";
  return p;
}

berth::ServiceRecord memos_record() {
  berth::ServiceRecord r;
  r.service_name = "berthpanel-memos";
  r.display_name = "Memos";
  r.description = "Notes";
  r.repo = "usememos/memos";
  r.commit = "abc123";
  r.port = 8234;
  return r;
}

}  // namespace

TEST(proxy_domain_accepts_valid_hostnames) {
  CHECK(validate_proxy_domain("example.com").is_ok());
  CHECK(validate_proxy_domain("sub.example.com").is_ok());
  CHECK(validate_proxy_domain("a-b.example.co.uk").is_ok());
  CHECK(validate_proxy_domain("Example.COM").is_ok());
  // Punycode label (an IDN in its ASCII form).
  CHECK(validate_proxy_domain("xn--p1ai.example").is_ok());
  CHECK(validate_proxy_domain("x1.example").is_ok());
}

TEST(proxy_domain_refuses_injection_and_junk) {
  CHECK(validate_proxy_domain("").is_err());
  CHECK(validate_proxy_domain("example").is_err());  // no dot
  CHECK(validate_proxy_domain("example.c").is_err());
  CHECK(validate_proxy_domain("1.2.3.4").is_err());  // IP, not a name
  CHECK(validate_proxy_domain("example.com.").is_err());
  CHECK(validate_proxy_domain("exa mple.com").is_err());
  CHECK(validate_proxy_domain("example.com ").is_err());
  CHECK(validate_proxy_domain("a.com\n}").is_err());
  CHECK(validate_proxy_domain("a}.com").is_err());
  CHECK(validate_proxy_domain("a{.com").is_err());
  CHECK(validate_proxy_domain("a.com\tsneaky").is_err());
  CHECK(validate_proxy_domain("-a.com").is_err());
  CHECK(validate_proxy_domain("a-.com").is_err());
  CHECK(validate_proxy_domain("a..com").is_err());
  CHECK(validate_proxy_domain("a.com;quote").is_err());
  CHECK(validate_proxy_domain(std::string(64, 'a') + ".com").is_err());
  // A raw U-label is refused: IDNs must be punycode.
  CHECK(validate_proxy_domain("ex\xc3\xa4mple.com").is_err());
}

TEST(proxy_domain_normalizes_case) {
  CHECK(berth::normalize_proxy_domain("ExAmPle.COM") == "example.com");
  CHECK(berth::normalize_proxy_domain("a-b.c") == "a-b.c");
}

TEST(proxy_reverse_proxy_is_loopback_only) {
  const auto ok = render_reverse_proxy_line("127.0.0.1", 8234);
  CHECK(ok.is_ok());
  CHECK(ok.value() == "reverse_proxy 127.0.0.1:8234");

  // Anything but loopback would expose a service the recipe bound to
  // localhost (the §8 loopback contract).
  CHECK(render_reverse_proxy_line("0.0.0.0", 8234).is_err());
  CHECK(render_reverse_proxy_line("localhost", 8234).is_err());
  CHECK(render_reverse_proxy_line("192.168.1.5", 8234).is_err());
  CHECK(render_reverse_proxy_line("127.0.0.1", 0).is_err());
}

TEST(proxy_route_block_is_a_valid_site_block) {
  const ProxyRoute route{.domain = "app.example.com",
                         .service_name = "berthpanel-memos",
                         .port = 8234};
  const auto block = render_route_block(route);
  CHECK(block.is_ok());
  CHECK(block.value() ==
        "app.example.com {\n\treverse_proxy 127.0.0.1:8234\n}\n");

  ProxyRoute bad = route;
  bad.domain = "app.example.com}\nadmin off";
  CHECK(render_route_block(bad).is_err());
}

TEST(proxy_caddyfile_sorts_and_disables_admin) {
  const std::vector<ProxyRoute> routes = {
      {.domain = "z.example.com", .service_name = "berthpanel-b", .port = 2},
      {.domain = "a.example.com", .service_name = "berthpanel-a", .port = 1},
  };
  const auto file =
      render_caddyfile(routes, "/home/u/.local/share/berthpanel/caddy");
  CHECK(file.is_ok());
  CHECK(file.value().find("admin off") != std::string::npos);
  CHECK(file.value().find("storage file_system "
                          "\"/home/u/.local/share/berthpanel/caddy\"") !=
        std::string::npos);
  // a.example.com must precede z.example.com regardless of input order.
  CHECK(file.value().find("a.example.com {") <
        file.value().find("z.example.com {"));
}

TEST(proxy_caddyfile_rejects_duplicate_domains) {
  const std::vector<ProxyRoute> routes = {
      {.domain = "a.example.com", .service_name = "berthpanel-a", .port = 1},
      {.domain = "a.example.com", .service_name = "berthpanel-b", .port = 2},
  };
  CHECK(render_caddyfile(routes, "/tmp/berthpanel-caddy").is_err());
}

TEST(proxy_caddyfile_quotes_a_storage_path_with_a_space) {
  const auto file =
      render_caddyfile({}, "/home/u/My Apps/.local/share/berthpanel/caddy");
  CHECK(file.is_ok());
  CHECK(
      file.value().find("\"/home/u/My Apps/.local/share/berthpanel/caddy\"") !=
      std::string::npos);
  // A relative data dir cannot be trusted to land where Caddy expects.
  CHECK(render_caddyfile({}, "relative/caddy").is_err());
}

TEST(proxy_upsert_is_idempotent_and_conflict_is_loud) {
  std::vector<ProxyRoute> routes;
  const auto added = berth::upsert_proxy_route(routes, "berthpanel-memos",
                                               "app.example.com", 8234);
  CHECK(added.is_ok());
  CHECK(added.value().size() == 1);

  // Same service + domain: update the port in place, no duplicate.
  const auto updated = berth::upsert_proxy_route(
      added.value(), "berthpanel-memos", "app.example.com", 9999);
  CHECK(updated.is_ok());
  CHECK(updated.value().size() == 1);
  CHECK(updated.value().front().port == 9999);

  // A different service claiming the same domain is a conflict.
  const auto clash = berth::upsert_proxy_route(
      updated.value(), "berthpanel-gitea", "app.example.com", 3000);
  CHECK(clash.is_err());
}

TEST(proxy_route_store_round_trips_and_is_sorted) {
  const TempDir tmp;
  const berth::Paths p = proxy_paths(tmp.path());
  const std::vector<ProxyRoute> routes = {
      {.domain = "z.example.com", .service_name = "berthpanel-b", .port = 2},
      {.domain = "a.example.com", .service_name = "berthpanel-a", .port = 1},
  };
  CHECK(berth::proxy_routes_store(p, routes).is_ok());
  const auto loaded = berth::proxy_routes_load(p);
  CHECK(loaded.is_ok());
  CHECK(loaded.value().size() == 2);
  CHECK(loaded.value().front().domain == "a.example.com");
  CHECK(loaded.value().front().port == 1);

  const auto dropped =
      berth::remove_proxy_route(loaded.value(), "a.example.com");
  CHECK(dropped.size() == 1);
  CHECK(berth::proxy_routes_store(p, dropped).is_ok());
  const auto reloaded = berth::proxy_routes_load(p);
  CHECK(reloaded.is_ok());
  CHECK(reloaded.value().size() == 1);
}

TEST(proxy_add_refuses_an_unregistered_service) {
  const TempDir tmp;
  const berth::Paths p = proxy_paths(tmp.path());
  const auto added =
      berth::proxy_add(p, "berthpanel-missing", "app.example.com", nullptr);
  CHECK(added.is_err());
}

TEST(proxy_add_refuses_an_injection_domain) {
  const TempDir tmp;
  const berth::Paths p = proxy_paths(tmp.path());
  CHECK(berth::registry_register(p, memos_record()).is_ok());
  const auto added = berth::proxy_add(p, "berthpanel-memos",
                                      "app.example.com}\nadmin off", nullptr);
  CHECK(added.is_err());
}

TEST(proxy_caddy_unit_renders_through_units) {
  const TempDir tmp;
  const berth::Paths p = proxy_paths(tmp.path());
  const auto spec = berth::caddy_unit_spec(p);
  CHECK(spec.is_ok());
  CHECK(spec.value().service_name == "berthpanel-caddy");
  CHECK(spec.value().limits.memory_max == "384M");
  CHECK(spec.value().limits.tasks_max == 512);

  const auto unit = berth::render_unit(spec.value());
  CHECK(unit.is_ok());
  CHECK(unit.value().find("ExecStart=") != std::string::npos);
  CHECK(unit.value().find("--adapter") != std::string::npos);
  CHECK(unit.value().find("MemoryMax=384M") != std::string::npos);
  CHECK(unit.value().find("NoNewPrivileges=yes") != std::string::npos);
  CHECK(unit.value().find("ReadWritePaths=") != std::string::npos);
}
