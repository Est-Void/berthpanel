#pragma once

// src/analyzer/ports.h — the listening-port parser behind System Scan, as a
// PURE function over the text of /proc/net/tcp and /proc/net/tcp6.
//
// Deliberately dumb, and the header says why: it reports ports that are in
// use, and NOTHING ELSE. There is no /proc/<pid> walk and no socket-inode
// lookup here, because attributing a listening port to a foreign process is a
// surveillance capability this report does not have (dev.md §7 — the old README
// claimed it did; the code refused to, and that restraint is the point). The
// report should be useless to anyone but the user.
//
// `owned` is not attribution: it is "the berthpanel registry already claims
// this port", which is our own record from state.json.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

struct ListeningPort {
  std::string address;  // "127.0.0.1", "::1", "0.0.0.0"
  std::uint16_t port = 0;
  bool ipv6 = false;
  bool owned = false;     // a berthpanel service in the registry claims it
  bool loopback = false;  // 127.0.0.0/8, ::1
  bool wildcard = false;  // 0.0.0.0 / :: — reachable from outside the host
};

// One parsed /proc/net/tcp[6] file. `unparsed` counts the lines the parser
// could not read: a silently shortened port list reads as "nothing is
// listening", which is exactly the kind of quiet wrong answer §11 is about.
struct ParsedPorts {
  std::vector<ListeningPort> ports;
  std::size_t unparsed = 0;
};

// Both files, merged and sorted. `ipv6_read`/`ipv6_error` say whether
// /proc/net/tcp6 was readable at all — a host without IPv6 is a fact the
// screen states, not an error.
struct PortReport {
  std::vector<ListeningPort> ports;
  std::size_t unparsed = 0;
  bool ipv6_read = false;
  std::string ipv6_error;
};

// Only state 0A (TCP_LISTEN) is reported; every other state is a connection,
// not something a "ports in use" report is about. Unparseable lines are
// counted and skipped, never fatal: the kernel owns this format and a new
// field must not blank the screen. `v6` selects the address rendering.
[[nodiscard]] Result<ParsedPorts> parse_proc_net(
    std::string_view content, bool v6,
    const std::vector<std::uint16_t>& owned_ports);

// Reads `proc_net_root/tcp` and `proc_net_root/tcp6`. ENOENT on tcp6 yields
// no IPv6 rows and no error; any other failure is an Err that names the file.
// The root is a parameter so the tests can point it at a fixture directory.
[[nodiscard]] Result<PortReport> listening_ports(
    const Paths& paths,
    const std::filesystem::path& proc_net_root = "/proc/net");

}  // namespace berth