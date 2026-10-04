
#include "state.h"

#include <algorithm>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "atomic.h"
#include "json.h"
#include "util/strutil.h"

namespace berth {

namespace {
constexpr std::string_view kUnitPrefix = "berthpanel-";
constexpr std::size_t kMaxUnitNameLen = 128;
constexpr int kRegistryVersion = 1;
}  // namespace

bool validate_unit_name(const std::string_view name) {
  if (name.size() > kMaxUnitNameLen) return false;
  if (!starts_with(name, kUnitPrefix)) return false;
  const std::string_view stem = name.substr(kUnitPrefix.size());
  if (stem.empty()) return false;
  for (const char c : stem) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '@';
    if (!ok) return false;
  }
  return true;
}

namespace {

[[noreturn]] void schema_fail(const std::string& why) {
  throw nlohmann::json::out_of_range::create(404, why, nullptr);
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
  const std::int64_t port = j.at("port").get<std::int64_t>();
  if (port < 0 || port > 65535) {
    schema_fail("port out of range: " + std::to_string(port));
  }
  r.port = static_cast<std::uint16_t>(port);
  if (!validate_unit_name(r.service_name)) {
    schema_fail("invalid service_name in registry: " + r.service_name);
  }
  return r;
}

Registry registry_from_json(const nlohmann::json& doc) {
  const nlohmann::json& services = doc.at("services");
  if (!services.is_array()) {
    throw nlohmann::json::type_error::create(
        302, "services is not an array", nullptr);
  }
  Registry reg;
  reg.services.reserve(services.size());
  for (const nlohmann::json& item : services) {
    reg.services.push_back(service_from_json(item));
  }
  return reg;
}

}  // namespace

Result<Registry> registry_load(const Paths& paths) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(paths.state_json, ec);
  if (ec) {
    return Error("registry_load: stat " + paths.state_json.string() + ": " +
                 ec.message());
  }
  if (!exists) return Registry{}; 

  Result<nlohmann::json> loaded = json_load(paths.state_json);
  if (loaded.is_err()) return std::move(loaded).error();  

  const auto schema_error = [&](const std::string& why) -> Result<Registry> {
    Result<std::filesystem::path> backup =
        quarantine_corrupt(paths.state_json);
    std::string msg =
        "registry_load: " + why + " in " + paths.state_json.string();
    if (backup.is_ok()) {
      msg += "; backed up to " + backup.value().string();
    } else {
      msg += "; AND the backup failed: " + backup.error().ctx();
    }
    return Error(std::move(msg));
  };

  const nlohmann::json& doc = loaded.value();
  if (!doc.is_object() || !doc.contains("version") ||
      !doc["version"].is_number_integer()) {
    return schema_error("not a versioned registry object");
  }
  if (doc["version"].get<int>() != kRegistryVersion) {
    return schema_error("unsupported registry version " +
                        doc["version"].dump());
  }
  try {
    Registry reg = registry_from_json(doc);
    std::sort(reg.services.begin(), reg.services.end(),
              [](const ServiceRecord& a, const ServiceRecord& b) {
                return a.service_name < b.service_name;
              });
    return reg;
  } catch (const nlohmann::json::exception& e) {
    return schema_error(e.what());
  }
}

Result<void> registry_store(const Paths& paths, const Registry& registry) {
  Result<void> dir = ensure_dir(paths.state_dir);
  if (dir.is_err()) return dir;

  Registry sorted = registry;
  std::sort(sorted.services.begin(), sorted.services.end(),
            [](const ServiceRecord& a, const ServiceRecord& b) {
              return a.service_name < b.service_name;
            });
  nlohmann::json services = nlohmann::json::array();
  for (const ServiceRecord& r : sorted.services) {
    services.push_back(record_to_json(r));
  }
  const nlohmann::json doc{{"version", kRegistryVersion},
                           {"services", services}};
  Result<void> written = json_store(paths.state_json, doc);
  if (written.is_err()) return written;
  return {};
}

Result<void> registry_register(const Paths& paths, const ServiceRecord& record) {
  if (!validate_unit_name(record.service_name)) {
    return Error("registry_register: invalid service name: '" +
                 record.service_name + "'");
  }
  Result<Registry> loaded = registry_load(paths);
  if (loaded.is_err()) return std::move(loaded).error();
  Registry reg = std::move(loaded).value();
  bool replaced = false;
  for (ServiceRecord& existing : reg.services) {
    if (existing.service_name == record.service_name) {
      existing = record;
      replaced = true;
      break;
    }
  }
  if (!replaced) reg.services.push_back(record);
  return registry_store(paths, reg);
}

Result<std::vector<ServiceRecord>> registry_list(const Paths& paths) {
  Result<Registry> loaded = registry_load(paths);
  if (loaded.is_err()) {
    return with_context(std::move(loaded).error(), "registry_list");
  }
  return std::move(loaded).value().services;
}

Result<std::optional<ServiceRecord>> registry_get(const Paths& paths,
                                                  std::string_view service_name) {
  Result<std::vector<ServiceRecord>> listed = registry_list(paths);
  if (listed.is_err()) {
    return with_context(std::move(listed).error(), "registry_get");
  }
  for (const ServiceRecord& r : listed.value()) {
    if (r.service_name == service_name) return std::optional<ServiceRecord>(r);
  }
  return std::optional<ServiceRecord>{};
}

Result<void> registry_unregister(const Paths& paths,
                                 std::string_view service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("registry_unregister: invalid service name: '" +
                 std::string(service_name) + "'");
  }
  Result<Registry> loaded = registry_load(paths);
  if (loaded.is_err()) return std::move(loaded).error();
  Registry reg = std::move(loaded).value();
  const auto it = std::find_if(
      reg.services.begin(), reg.services.end(),
      [&](const ServiceRecord& r) {
        return r.service_name == service_name;
      });
  if (it == reg.services.end()) {
    return Error("registry_unregister: unknown service: " +
                 std::string(service_name));
  }
  reg.services.erase(it);
  return registry_store(paths, reg);
}

}  // namespace berth
