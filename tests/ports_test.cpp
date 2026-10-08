// Tests for the System Scan port parser (stage 6.5). The parser is PURE, so
// every case is a fixture string — including the ones /proc cannot be asked
// for on this machine. The format is the kernel's, so the fixtures below are
// verbatim rows from /proc/net/tcp and /proc/net/tcp6.

#include "analyzer/ports.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

namespace {

// Header + three rows: two LISTEN, one ESTABLISHED (which must NOT appear).
// Raw strings because these are fixtures, not code: adjacent "literals" would
// be concatenated into one line and the parser would read a single row out of
// a three-row file — which is exactly the kind of test that passes for the
// wrong reason.
constexpr const char* kTcp4 =
    R"(  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 24370 2 0000000009c7ec4f 100 0 0 10 0
   1: 1C00A8C0:BB14 29A79A95:01BB 01 00000000:00000000 00:00000000 00000000  1000        0 2451829 1 00000000d0e5de8c 31 4 28 12 -1
   2: 00000000:C1FA 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 12347 1 0000000000000000 100 0 0 10 0
)";

// ::1 (the last word holds the 01, which is what makes it ::1 and not
// ::0100:0001) and :: listening.
constexpr const char* kTcp6 =
    R"(  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000000000000000000001000000:1F90 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2238711 1 00000000bc6f13e5 100 0 0 10 0
   1: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2238712 1 0000000000000000 100 0 0 10 0
)";

berth::Result<berth::ParsedPorts> v4(const std::string& text,
                                     const std::vector<std::uint16_t>& owned) {
  return berth::parse_proc_net(text, false, owned);
}

}  // namespace

TEST(ports_only_listening_sockets_are_reported) {
  const berth::Result<berth::ParsedPorts> parsed = v4(kTcp4, {});
  CHECK(parsed.is_ok());
  CHECK(parsed.value().ports.size() == 2);  // the ESTABLISHED row is not a port
  CHECK(parsed.value().unparsed == 0);
  CHECK(parsed.value().ports[0].port == 8080);   // 0x1F90
  CHECK(parsed.value().ports[1].port == 49658);  // 0xC1FA
}

TEST(ports_render_the_hex_addresses_human_readably) {
  const berth::Result<berth::ParsedPorts> parsed = v4(kTcp4, {});
  CHECK(parsed.is_ok());
  // "0100007F" is one little-endian word: 127.0.0.1, not 1.0.0.127.
  CHECK(parsed.value().ports[0].address == "127.0.0.1");
  CHECK(parsed.value().ports[0].loopback);
  CHECK(!parsed.value().ports[0].wildcard);
  CHECK(parsed.value().ports[0].ipv6 == false);
  // "00000000" is the wildcard address — reachable from every interface.
  CHECK(parsed.value().ports[1].address == "0.0.0.0");
  CHECK(parsed.value().ports[1].wildcard);
  CHECK(!parsed.value().ports[1].loopback);
}

TEST(ports_ipv6_fixtures_decode_including_loopback_and_wildcard) {
  const berth::Result<berth::ParsedPorts> parsed =
      berth::parse_proc_net(kTcp6, true, {});
  CHECK(parsed.is_ok());
  CHECK(parsed.value().ports.size() == 2);
  // The all-zero group is ::1 here, because the word order is little-endian
  // per 32-bit word: getting this backwards yields "::100" and a plausible
  // looking lie.
  CHECK(parsed.value().ports[0].address == "::1");
  CHECK(parsed.value().ports[0].port == 8080);
  CHECK(parsed.value().ports[0].loopback);
  CHECK(parsed.value().ports[0].ipv6);
  CHECK(parsed.value().ports[1].address == "::");
  CHECK(parsed.value().ports[1].port == 22);
  CHECK(parsed.value().ports[1].wildcard);
}

TEST(ports_mark_the_ports_the_registry_already_claims) {
  // `owned` is OUR OWN record (state.json), not attribution of a foreign
  // listener: the parser is handed the numbers and never goes looking (§7).
  const berth::Result<berth::ParsedPorts> parsed = v4(kTcp4, {8080, 49658});
  CHECK(parsed.is_ok());
  CHECK(parsed.value().ports[0].owned);
  CHECK(parsed.value().ports[1].owned);

  const berth::Result<berth::ParsedPorts> unowned = v4(kTcp4, {1});
  CHECK(unowned.value().ports[0].owned == false);
  CHECK(unowned.value().ports[1].owned == false);
}

TEST(ports_count_unreadable_lines_instead_of_pretending_they_are_empty) {
  // A silently dropped row reads as "nothing is listening on that port", which
  // is the kind of quiet wrong answer §11 is about.
  const berth::Result<berth::ParsedPorts> parsed = v4(
      R"(  sl  local_address rem_address   st
   0: 0100007F:1F90 00000000:0000 0A
   1: garbage line without columns
   2: 0100007F:ZZZZ 00000000:0000 0A
   3: 0100007F:1F91 00000000:0000 0A
)",
      {});
  CHECK(parsed.is_ok());
  // Both good rows survive; the garbage line and the non-hex port are counted
  // as unreadable rather than quietly dropped.
  CHECK(parsed.value().ports.size() == 2);
  CHECK(parsed.value().ports[0].port == 8080);
  CHECK(parsed.value().ports[1].port == 8081);
  CHECK(parsed.value().unparsed == 2);
}

TEST(ports_an_empty_file_is_empty_and_not_an_error) {
  const berth::Result<berth::ParsedPorts> parsed = v4("", {});
  CHECK(parsed.is_ok());
  CHECK(parsed.value().ports.empty());
  CHECK(parsed.value().unparsed == 0);
}

TEST(ports_reads_both_kernel_files_from_a_fixture_directory) {
  const TempDir tmp;
  const std::filesystem::path root = tmp.path() / "net";
  CHECK(berth::ensure_dir(root).is_ok());
  CHECK(berth::atomic_write(root / "tcp", kTcp4).is_ok());

  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  paths.state_json = paths.state_dir / "state.json";
  CHECK(berth::ensure_dir(paths.state_dir).is_ok());

  const berth::Result<berth::PortReport> report =
      berth::listening_ports(paths, root);
  CHECK(report.is_ok());
  CHECK(report.value().ports.size() == 2);
  CHECK(report.value().unparsed == 0);
  // No tcp6 in the fixture: that is a FACT the report carries, not a failure.
  CHECK(!report.value().ipv6_read);
  CHECK(!report.value().ipv6_error.empty());
}

TEST(ports_sorted_by_port_so_the_list_is_stable) {
  const TempDir tmp;
  const std::filesystem::path root = tmp.path() / "net";
  CHECK(berth::ensure_dir(root).is_ok());
  CHECK(
      berth::atomic_write(
          root / "tcp",
          R"(  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000:5000 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1 1 0 0 10 0
   1: 0100007F:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2 1 0 0 10 0
)")
          .is_ok());
  CHECK(berth::atomic_write(root / "tcp6", kTcp6).is_ok());

  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  paths.state_json = paths.state_dir / "state.json";
  CHECK(berth::ensure_dir(paths.state_dir).is_ok());

  const berth::Result<berth::PortReport> report =
      berth::listening_ports(paths, root);
  CHECK(report.is_ok());
  CHECK(report.value().ipv6_read);
  CHECK(report.value().ports.size() == 4);
  for (std::size_t i = 1; i < report.value().ports.size(); ++i) {
    CHECK(report.value().ports[i - 1].port <= report.value().ports[i].port);
  }
  CHECK(report.value().ports.front().port == 22);
}

TEST(ports_a_missing_tcp_file_is_a_loud_error_not_an_empty_report) {
  const TempDir tmp;
  const std::filesystem::path root = tmp.path() / "net";
  CHECK(berth::ensure_dir(root).is_ok());

  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  paths.state_json = paths.state_dir / "state.json";
  CHECK(berth::ensure_dir(paths.state_dir).is_ok());

  const berth::Result<berth::PortReport> report =
      berth::listening_ports(paths, root);
  CHECK(report.is_err());
  CHECK(report.error().ctx().find("tcp") != std::string::npos);
}