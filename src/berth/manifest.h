#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "../util/result.h"
#include "recipes.h"

namespace berth {

// READ-ONLY: a bad manifest must never be quarantined or rewritten.
[[nodiscard]] Result<Recipe> load_manifest(const std::filesystem::path& path);
[[nodiscard]] std::string render_manifest_plan(const Recipe& recipe);
[[nodiscard]] nlohmann::json manifest_json(const Recipe& recipe);

}
