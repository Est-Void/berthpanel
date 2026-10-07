#pragma once

// src/berth/userrecipes.h — storage for BYO recipes (plan stage 4.19):
// one JSON file per recipe under paths.recipes_dir, named
// <service_name>.json.
//
// The ONLY validator is validate_recipe() from recipes.cpp — the same one
// the builtin catalog passes at load (§11: two validators that disagree).
// A file that fails to parse or to validate is a LOUD error naming the
// file and the violated rule; it is never skipped and never read as
// absent. service_name gets the `berthpanel-` prefix added on store when
// missing, then the shared validate_unit_name enforces it like everywhere
// else. Recipes hold env KEYS, never values — secrets live in the
// 0600 secrets file at deploy time (secrets.cpp).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "recipes.h"

namespace berth {

// Upsert: an existing recipe with the same service_name is replaced.
// The service name is normalized with the mandatory prefix before
// validation and before the file name is derived from it.
[[nodiscard]] Result<std::filesystem::path> user_recipe_store(
    const Paths& paths, const Recipe& recipe);

// Loads EVERY recipe file. One bad file fails the whole load — a corrupt
// recipe must not silently disappear from listings (§11).
[[nodiscard]] Result<std::vector<Recipe>> user_recipes_load(const Paths& paths);

// Ok(nullopt) when no recipe with this name is stored.
[[nodiscard]] Result<std::optional<Recipe>> user_recipe_get(
    const Paths& paths, std::string_view service_name);

// Idempotent: Ok(false) when nothing was stored under this name.
[[nodiscard]] Result<bool> user_recipe_remove(const Paths& paths,
                                              std::string_view service_name);

[[nodiscard]] std::string render_user_recipes_list(
    const std::vector<Recipe>& recipes);

}  // namespace berth
