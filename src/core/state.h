#pragma once

// src/core/state.h — the service registry: what the panel has deployed.
// state.json is a versioned list of ServiceRecord keyed by service_name;
// the storage discipline (quarantine on corrupt, sort by key) is
// docstore.h's, and the name policy is core/names.h's. This header owns only
// the record shape and the registry operations.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/paths.h"
#include "util/result.h"

namespace berth {

struct ServiceRecord {
  std::string service_name;
  std::string display_name;
  std::string description;
  std::string repo;
  std::string commit;
  std::uint16_t port = 0;
};

struct Registry {
  std::vector<ServiceRecord> services;
};

[[nodiscard]] Result<Registry> registry_load(const Paths& paths);
[[nodiscard]] Result<void> registry_store(const Paths& paths,
                                          const Registry& registry);

[[nodiscard]] Result<void> registry_register(const Paths& paths,
                                             const ServiceRecord& record);

[[nodiscard]] Result<std::optional<ServiceRecord>> registry_get(
    const Paths& paths, std::string_view service_name);

[[nodiscard]] Result<std::vector<ServiceRecord>> registry_list(
    const Paths& paths);

[[nodiscard]] Result<void> registry_unregister(const Paths& paths,
                                               std::string_view service_name);

// The `--json` document for the registry (plan 5.3): the versioned envelope
// plus a `services` array sorted by service_name, so the bytes are stable.
[[nodiscard]] nlohmann::json services_json(
    const std::vector<ServiceRecord>& services);

}  // namespace berth
