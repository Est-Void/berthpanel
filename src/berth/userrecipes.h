#pragma once

// One JSON file per recipe under paths.recipes_dir, named <service_name>.json.
// The ONLY validator is validate_recipe(); a file that fails to parse or to
// validate is a LOUD error naming the file, never read as absent (§11).
// service_name gets the mandatory prefix on store; a name that fails
// validate_unit_name on load is an Err, never a silent rename.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "recipes.h"

namespace berth {

// Without `force`, a name already taken (user recipe or builtin) is an Err:
// silently replacing either hides what deploys.
[[nodiscard]] Result<std::filesystem::path> user_recipe_store(
    const Paths& paths, const Recipe& recipe, bool force);

// One bad file fails the whole load; the file name IS the service name.
[[nodiscard]] Result<std::vector<Recipe>> user_recipes_load(const Paths& paths);

[[nodiscard]] Result<std::optional<Recipe>> user_recipe_get(
    const Paths& paths, std::string_view service_name);

[[nodiscard]] Result<bool> user_recipe_remove(const Paths& paths,
                                              std::string_view service_name);

[[nodiscard]] std::string render_user_recipes_list(
    const std::vector<Recipe>& recipes);

[[nodiscard]] nlohmann::json user_recipes_json(
    const std::vector<Recipe>& recipes);

}
