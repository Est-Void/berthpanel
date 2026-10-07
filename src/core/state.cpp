// src/core/state.cpp — see state.h for the contract.

#include "state.h"

#include <algorithm>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/docstore.h"
#include "core/names.h"
#include "core/paths.h"

namespace berth {

namespace {

constexpr int kRegistryVersion = 1;

[[noreturn]] void schema_fail(const std::string& why) {
  throw nlohmann::json::out_of_range::create(404, why, nullptr);
}

std::uint16_t port_from_json(const nlohmann::json& j) {
  const std::int64_t port = j.get<std::int64_t>();
  if (port < 0 || port > 65535) {
    schema_fail("port out of range: " + std::to_string(port));
  }
  return static_cast<std::uint16_t>(port);
}

nlohmann::json record_to_json(const ServiceRecord& r) {
  return nlohmann::json{{"service_name", r.service_name},
                        {"display_name", r.display_name},
                        {"description", r.description},
                        {"repo", r.repo},
                        {"commit", r.commit},
                        {"port", r.port}};
}

ServiceRecord service_from_json(const nlohmann::json& j) {
  ServiceRecord r;
  r.service_name = j.at("service_name").get<std::string>();
  r.display_name = j.at("display_name").get<std::string>();
  r.description = j.at("description").get<std::string>();
  r.repo = j.at("repo").get<std::string>();
  r.commit = j.at("commit").get<std::string>();
  r.port = port_from_json(j.at("port"));
  if (!validate_unit_name(r.service_name)) {
    schema_fail("invalid service_name in registry: " + r.service_name);
  }
  return r;
}

std::string record_key(const ServiceRecord& r) {
  return r.service_name;
}

void replace_or_append(std::vector<ServiceRecord>* services,
                       const ServiceRecord& record) {
  for (ServiceRecord& existing : *services) {
    if (existing.service_name != record.service_name)
      continue;
    existing = record;
    return;
  }
  services->push_back(record);
}

bool erase_named(std::vector<ServiceRecord>* services,
                 const std::string_view name) {
  const auto it = std::find_if(
      services->begin(), services->end(),
      [name](const ServiceRecord& r) { return r.service_name == name; });
  if (it == services->end())
    return false;
  services->erase(it);
  return true;
}

DocStore<ServiceRecord> store_for(const Paths& paths) {
  return DocStore<ServiceRecord>(paths.state_json, paths.state_dir,
                                 kRegistryVersion, "services", record_to_json,
                                 service_from_json, record_key);
}

}  // namespace

Result<Registry> registry_load(const Paths& paths) {
  Result<std::vector<ServiceRecord>> loaded = store_for(paths).load();
  if (loaded.is_err()) {
    return with_context(std::move(loaded).error(), "registry_load");
  }
  Registry reg;
  reg.services = std::move(loaded).value();
  return reg;
}

Result<void> registry_store(const Paths& paths, const Registry& registry) {
  return store_for(paths).store(registry.services);
}

Result<void> registry_register(const Paths& paths,
                               const ServiceRecord& record) {
  if (!validate_unit_name(record.service_name)) {
    return Error("registry_register: invalid service name: '" +
                 record.service_name + "'");
  }
  Result<std::vector<ServiceRecord>> loaded = registry_list(paths);
  if (loaded.is_err())
    return std::move(loaded).error();
  std::vector<ServiceRecord> services = std::move(loaded).value();
  replace_or_append(&services, record);
  return registry_store(paths, Registry{.services = std::move(services)});
}

Result<std::vector<ServiceRecord>> registry_list(const Paths& paths) {
  Result<Registry> loaded = registry_load(paths);
  if (loaded.is_err()) {
    return with_context(std::move(loaded).error(), "registry_list");
  }
  return std::move(loaded).value().services;
}

Result<std::optional<ServiceRecord>> registry_get(
    const Paths& paths, const std::string_view service_name) {
  Result<std::vector<ServiceRecord>> listed = registry_list(paths);
  if (listed.is_err()) {
    return with_context(std::move(listed).error(), "registry_get");
  }
  for (const ServiceRecord& r : listed.value()) {
    if (r.service_name == service_name)
      return std::optional<ServiceRecord>(r);
  }
  return std::optional<ServiceRecord>{};
}

Result<void> registry_unregister(const Paths& paths,
                                 const std::string_view service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("registry_unregister: invalid service name: '" +
                 std::string(service_name) + "'");
  }
  Result<std::vector<ServiceRecord>> listed = registry_list(paths);
  if (listed.is_err())
    return std::move(listed).error();
  std::vector<ServiceRecord> services = std::move(listed).value();
  const bool found = erase_named(&services, service_name);
  if (!found) {
    return Error("registry_unregister: unknown service: " +
                 std::string(service_name));
  }
  return registry_store(paths, Registry{.services = std::move(services)});
}

}  // namespace berth
