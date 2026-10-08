// src/analyzer/ports.cpp — see ports.h for the contract.

#include "ports.h"

#include <algorithm>
#include <string>

#include "../core/state.h"
#include "../util/fs.h"
#include "../util/strutil.h"

namespace berth {

namespace {

// /proc/net/tcp state column: 0A is TCP_LISTEN. Everything else (01
// ESTABLISHED, 06 TIME_WAIT, ...) is a connection, not a listening socket.
constexpr std::string_view kStateListen = "0A";

// One hex byte, or -1. Hand-rolled because std::stoi would throw (exceptions
// are for programmer error, and malformed kernel output is not that) and
// strtol would accept leading junk.
int hex_byte(const char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return -1;
}

// Two lowercase hex digits, zero padded. Hand-rolled because a byte is not a
// string: std::to_string(10) is "10", which silently changes the width of every
// address built from it.
std::string to_hex(const int value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(2, '0');
  out[0] = kDigits[(value >> 4) & 0xf];
  out[1] = kDigits[value & 0xf];
  return out;
}

// The port part of "0100007F:1F90". 0 means "no port", which /proc/net/tcp
// uses only for a listening socket with no bound port — not a thing.
std::uint16_t parse_port(const std::string_view hex) {
  if (hex.size() != 4) {
    return 0;
  }
  unsigned value = 0;
  for (const char digit : hex) {
    const int nibble = hex_byte(digit);
    if (nibble < 0) {
      return 0;
    }
    value = (value << 4) | static_cast<unsigned>(nibble);
  }
  return static_cast<std::uint16_t>(value);
}

// IPv4 in /proc is ONE 32-bit word printed from memory, so the byte pairs come
// out in reverse: "0100007F" is 127.0.0.1, not 1.0.0.127. Reading them in the
// written order is the bug that makes the whole section quietly wrong.
std::string parse_ipv4(const std::string_view hex) {
  if (hex.size() != 8) {
    return {};
  }
  std::string out;
  for (int word = 3; word >= 0; --word) {
    const std::string_view byte_hex =
        hex.substr(static_cast<std::size_t>(word) * 2, 2);
    const int high = hex_byte(byte_hex[0]);
    const int low = hex_byte(byte_hex[1]);
    if (high < 0 || low < 0) {
      return {};
    }
    if (word != 3) {
      out += '.';
    }
    out += std::to_string(high * 16 + low);
  }
  return out;
}

// IPv6 is four 32-bit words, each printed from memory, so the bytes come out
// reversed INSIDE each word: the last word of ::1 is "01000000". Only the
// compressed forms the screen needs: ::1, :: and the general case.
std::string parse_ipv6(const std::string_view hex) {
  if (hex.size() != 32) {
    return {};
  }
  std::string bytes;  // two hex digits per byte, 32 characters
  for (int word = 0; word < 4; ++word) {
    const std::string_view chunk =
        hex.substr(static_cast<std::size_t>(word) * 8, 8);
    for (int byte = 3; byte >= 0; --byte) {
      const std::string_view pair =
          chunk.substr(static_cast<std::size_t>(byte) * 2, 2);
      const int high = hex_byte(pair[0]);
      const int low = hex_byte(pair[1]);
      if (high < 0 || low < 0) {
        return {};
      }
      bytes += to_hex(high * 16 + low);
    }
  }
  if (bytes == std::string(31, '0') + "1") {
    return "::1";
  }
  if (bytes == std::string(32, '0')) {
    return "::";
  }
  std::string out;
  for (std::size_t byte = 0; byte < 16; ++byte) {
    if (byte > 0 && byte % 2 == 0) {
      out += ':';
    }
    out += bytes.substr(byte * 2, 2);
  }
  return out;
}

bool is_loopback(const std::string& address) {
  return address == "::1" || address.rfind("127.", 0) == 0;
}

}  // namespace

Result<ParsedPorts> parse_proc_net(
    const std::string_view content, const bool v6,
    const std::vector<std::uint16_t>& owned_ports) {
  ParsedPorts parsed;
  std::size_t line_number = 0;
  for (const std::string& raw : berth::split(content, "\n")) {
    ++line_number;
    if (line_number == 1) {
      continue;  // the column header the kernel prints
    }
    const std::string_view line = berth::trim(raw);
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string> fields = berth::split(line, " ");
    //   sl  local_address rem_address st tx:rx tr:when retrnsmt uid timeout
    // A row that does not have at least local_address + rem_address + st is
    // not a row this parser understands.
    if (fields.size() < 4) {
      ++parsed.unparsed;
      continue;
    }
    // split() keeps empty fields, so consecutive spaces yield "". Drop them
    // before indexing: the kernel pads with spaces and we index by position.
    std::vector<std::string> columns;
    columns.reserve(fields.size());
    for (const std::string& field : fields) {
      if (!field.empty()) {
        columns.push_back(field);
      }
    }
    if (columns.size() < 4) {
      ++parsed.unparsed;
      continue;
    }
    // The state column must LOOK like a state: two hex digits. A row we cannot
    // even identify is counted rather than treated as "not listening", because
    // "not listening" and "not understood" are different claims.
    if (columns[3].size() != 2 || hex_byte(columns[3][0]) < 0 ||
        hex_byte(columns[3][1]) < 0) {
      ++parsed.unparsed;
      continue;
    }
    if (columns[3] != kStateListen) {
      continue;
    }
    const std::vector<std::string> local = berth::split(columns[1], ":");
    if (local.size() != 2) {
      ++parsed.unparsed;
      continue;
    }
    ListeningPort entry;
    entry.port = parse_port(local[1]);
    entry.address = v6 ? parse_ipv6(local[0]) : parse_ipv4(local[0]);
    if (entry.port == 0 || entry.address.empty()) {
      ++parsed.unparsed;
      continue;
    }
    entry.ipv6 = v6;
    entry.loopback = is_loopback(entry.address);
    entry.wildcard = entry.address == "0.0.0.0" || entry.address == "::";
    entry.owned = std::find(owned_ports.begin(), owned_ports.end(),
                            entry.port) != owned_ports.end();
    parsed.ports.push_back(std::move(entry));
  }
  return parsed;
}

Result<PortReport> listening_ports(const Paths& paths,
                                   const std::filesystem::path& proc_net_root) {
  // Our own ports, from our own registry: this is the ONE thing the port
  // section is allowed to know about a listener, and it knows it from
  // state.json rather than by asking the kernel who owns it (§7).
  std::vector<std::uint16_t> owned;
  Result<std::vector<ServiceRecord>> registered = registry_list(paths);
  if (registered.is_ok()) {
    for (const ServiceRecord& record : registered.value()) {
      if (record.port != 0) {
        owned.push_back(record.port);
      }
    }
  }

  PortReport report;
  const Result<std::string> tcp = read_file(proc_net_root / "tcp");
  if (tcp.is_err()) {
    return Error("scan: reading " + (proc_net_root / "tcp").string() +
                 " failed: " + tcp.error().ctx());
  }
  Result<ParsedPorts> parsed_v4 = parse_proc_net(tcp.value(), false, owned);
  if (parsed_v4.is_err()) {
    return std::move(parsed_v4).error();
  }
  ParsedPorts v4 = std::move(parsed_v4).value();
  report.ports = std::move(v4.ports);
  report.unparsed = v4.unparsed;

  // No IPv6 on this host is a fact to state, not a failure: the screen shows
  // the section and says the file was not there.
  const Result<std::string> tcp6 = read_file(proc_net_root / "tcp6");
  if (tcp6.is_err()) {
    report.ipv6_error = tcp6.error().ctx();
    return report;
  }
  Result<ParsedPorts> parsed_v6 = parse_proc_net(tcp6.value(), true, owned);
  if (parsed_v6.is_err()) {
    return std::move(parsed_v6).error();
  }
  report.ipv6_read = true;
  ParsedPorts v6 = std::move(parsed_v6).value();
  report.unparsed += v6.unparsed;
  for (ListeningPort& entry : v6.ports) {
    report.ports.push_back(std::move(entry));
  }

  std::sort(report.ports.begin(), report.ports.end(),
            [](const ListeningPort& a, const ListeningPort& b) {
              if (a.port != b.port) {
                return a.port < b.port;
              }
              return a.address < b.address;
            });
  return report;
}

}  // namespace berth