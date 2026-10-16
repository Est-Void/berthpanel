#pragma once

// No pid attribution: that is surveillance; `owned` = our own registry (§7).

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
  std::string address;
  std::uint16_t port = 0;
  bool ipv6 = false;
  bool owned = false;
  bool loopback = false;  // 127.0.0.0/8, ::1
  bool wildcard = false;  // 0.0.0.0 / :: — reachable from outside the host
};

// `unparsed`: a silently shortened list reads as "nothing is listening" (§11).
struct ParsedPorts {
  std::vector<ListeningPort> ports;
  std::size_t unparsed = 0;
};

// `ipv6_read`/`ipv6_error`: a host without IPv6 is a fact, not an error.
struct PortReport {
  std::vector<ListeningPort> ports;
  std::size_t unparsed = 0;
  bool ipv6_read = false;
  std::string ipv6_error;
};

// Only state 0A; unparseable lines are counted, never fatal (kernel's format).
[[nodiscard]] Result<ParsedPorts> parse_proc_net(
    std::string_view content, bool v6,
    const std::vector<std::uint16_t>& owned_ports);

// ENOENT on tcp6 gives no rows and no error; other failures name the file.
[[nodiscard]] Result<PortReport> listening_ports(
    const Paths& paths,
    const std::filesystem::path& proc_net_root = "/proc/net");

}
