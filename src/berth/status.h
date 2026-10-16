#pragma once

// READ-ONLY: no route starts, stops, deploys or writes. NO ATTRIBUTION: a port
// is reported with whether the registry claims it, never the process behind it.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "serve.h"

namespace berth {

struct StatusPort {
  std::string address;
  std::uint16_t port = 0;
  bool loopback = false;
  bool wildcard = false;
  bool owned = false;
};

struct StatusService {
  std::string service_name;
  std::string display_name;
  std::string description;
  std::string repo;
  std::string commit;
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
  // The running/interrupted split needs the deploy lock, which a read-only page
  // must not take: that could race a real deploy.
  std::vector<StatusDeploy> open_deploys;
  // A section that could not be read: say so, never render it as an empty list.
  std::string error;
};

[[nodiscard]] nlohmann::json status_json(const StatusSnapshot& snapshot);

// The minimal HTML page. Every value that can be user-controlled is escaped.
[[nodiscard]] std::string render_status_html(const StatusSnapshot& snapshot);

// `provider` runs on EVERY request; it must be pure enough for the accept loop.
[[nodiscard]] RequestHandler make_status_handler(
    std::function<StatusSnapshot()> provider);

}
