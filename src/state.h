#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "paths.h"
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

[[nodiscard]] bool validate_unit_name(std::string_view name);

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

[[nodiscard]] Result<std::vector<ServiceRecord>> registry_list(const Paths& paths);

[[nodiscard]] Result<void> registry_unregister(const Paths& paths,
                                               std::string_view service_name);

}  // namespace berth
