#pragma once

// src/berth/catalog.h — the curated builtin catalog: recipes as DATA over
// the recipe schema (dev.md §8). Every entry must pass validate_recipe()
// at load; the catalog contract tests assert the same rules over the whole
// table. Catalog contents grow in waves; wave 1 ships Memos, SearXNG and
// VERT arrive last (docs/plan.md «Каталог»).

#include <string_view>
#include <vector>

#include "../util/result.h"
#include "recipes.h"

namespace berth {

[[nodiscard]] Result<std::vector<Recipe>> builtin_catalog();

[[nodiscard]] Result<Recipe> builtin_recipe(std::string_view service_name);

[[nodiscard]] std::string render_catalog_list();

}  // namespace berth