// src/core/json.cpp — see json.h for the contract.

#include "json.h"

#include <string>
#include <utility>

#include "core/atomic.h"
#include "util/fs.h"

namespace berth {

Result<std::filesystem::path> quarantine_corrupt(
    const std::filesystem::path& path) {
  Result<std::filesystem::path> backup = backup_unique(path);
  if (backup.is_err()) {
    return with_context(std::move(backup), "quarantine_corrupt");
  }
  return backup;
}

Result<nlohmann::json> json_load(const std::filesystem::path& path) {
  Result<std::string> raw = read_file(path);
  if (raw.is_err()) {
    return with_context(std::move(raw).error(),
                        "json_load: reading " + path.string());
  }
  try {
    return nlohmann::json::parse(raw.value());
  } catch (const nlohmann::json::parse_error& e) {
    Result<std::filesystem::path> backup = quarantine_corrupt(path);
    std::string msg = "json_load: parse " + path.string() + ": " + e.what();
    if (backup.is_ok()) {
      msg += "; corrupt file backed up to " + backup.value().string();
    } else {
      msg += "; AND the backup failed: " + backup.error().ctx();
    }
    return Error(std::move(msg));
  }
}

Result<void> json_store(const std::filesystem::path& path,
                        const nlohmann::json& value) {
  const std::string text = value.dump(2) + "\n";
  Result<void> written = atomic_write(path, text, kPermPrivate);
  if (written.is_err()) {
    return with_context(std::move(written).error(),
                        "json_store: " + path.string());
  }
  return {};
}

}  // namespace berth
