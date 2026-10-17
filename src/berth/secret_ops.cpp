#include "secret_ops.h"

#include <filesystem>
#include <optional>
#include <utility>

#include "../core/names.h"
#include "../core/state.h"
#include "secrets.h"

namespace berth {

Result<void> rotate_secret(const Paths& paths, const std::string& service_name,
                           const std::string& key, const std::string& value,
                           const ServiceRestart& restart) {
  if (!validate_unit_name(service_name)) {
    return Error("secrets: invalid service name: '" + service_name + "'");
  }
  Result<std::optional<ServiceRecord>> record =
      registry_get(paths, service_name);
  if (record.is_err()) {
    return with_context(std::move(record).error(),
                        "secrets: checking " + service_name);
  }
  if (!record.value().has_value()) {
    return Error("secrets: '" + service_name +
                 "' is not registered; deploy it before rotating a secret");
  }
  Result<std::filesystem::path> written =
      set_environment_value(paths, service_name, key, value);
  if (written.is_err()) {
    return with_context(std::move(written).error(),
                        "secrets: rotating " + key + " for " + service_name);
  }
  Result<void> restarted = restart(service_name);
  if (restarted.is_err()) {
    return with_context(
        std::move(restarted).error(),
        "secrets: the new value is written, but restarting " + service_name +
            " failed");
  }
  return {};
}

}
