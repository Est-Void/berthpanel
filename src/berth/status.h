#pragma once

// src/berth/status.h — the read-only status page behind __serve-status
// (plan 5.4). It reuses the loopback-only server from serve.h and adds ONE
// rule of its own: it never acts and never attributes.
//
//   * READ-ONLY. There is no route that starts, stops, deploys or writes
//     anything; the page only reports what the registry, the dumb port scan
//     and the deploy journal already say.
//   * NO ATTRIBUTION. Ports come from analyzer/ports.cpp, which reports a
//     port and whether the berthpanel registry claims it — never a foreign
//     process (§7 calls that surveillance).
//
// A snapshot is collected per request (so the page is current) and rendered
// by pure functions, which is what makes the body testable without a socket.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "serve.h"

namespace berth {

struct StatusPort {
  std::string address;  // "127.0.0.1", "::1", "0.0.0.0"
  std::uint16_t port = 0;
  bool loopback = false;
  bool wildcard = false;  // reachable from outside the host
  bool owned = false;     // a berthpanel service in the registry claims it
};

struct StatusService {
  std::string service_name;
  std::string display_name;
  std::string description;
  std::string repo;
  std::string commit;  // full pin; the HTML renderer shortens it
  std::uint16_t port = 0;
};

struct StatusDeploy {
  std::string service_name;
  std::string stage;
  std::int64_t started_at_unix = 0;
};

struct StatusSnapshot {
  std::vector<StatusService> services;
  std::vector<StatusPort> ports;
  // Open journal entries: a deploy that has not reached a terminal outcome.
  // The running/interrupted split needs the deploy lock, which a read-only
  // page deliberately does not touch (acquiring it could race a real deploy).
  std::vector<StatusDeploy> open_deploys;
  // Set when a section could not be read; the page says so instead of
  // rendering an empty list that reads as "nothing here" (§11).
  std::string error;
};

// The `?format=json` / `/status.json` document: the versioned envelope plus
// services, ports and open deploys.
[[nodiscard]] nlohmann::json status_json(const StatusSnapshot& snapshot);

// The minimal HTML page. Every value that can be user-controlled is escaped.
[[nodiscard]] std::string render_status_html(const StatusSnapshot& snapshot);

// Builds the request handler. `provider` runs on EVERY request so the page is
// current; it must be pure enough to call from the accept loop.
[[nodiscard]] RequestHandler make_status_handler(
    std::function<StatusSnapshot()> provider);

}  // namespace berth
