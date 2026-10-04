#pragma once
#include <filesystem>

#include "util/result.h"
#include <nlohmann/json.hpp>

namespace berth {

[[nodiscard]] Result<nlohmann::json> json_load(const std::filesystem::path& path);

[[nodiscard]] Result<void> json_store(const std::filesystem::path& path,
                                      const nlohmann::json& value);

[[nodiscard]] Result<std::filesystem::path> quarantine_corrupt(
    const std::filesystem::path& path);

}  // namespace berth
