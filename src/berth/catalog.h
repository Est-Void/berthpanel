#pragma once

// src/berth/catalog.h — the curated builtin catalog: recipes as DATA over
// the recipe schema (dev.md §8). Every entry must pass validate_recipe()
// at load; the catalog contract tests assert the same rules over the whole
// table. Catalog contents grow in waves; wave 1 ships Memos, SearXNG and
// VERT arrive last (docs/plan.md «Каталог»).
//
// resolve_recipe() is the ONE entry point deploys use to turn a service
// name into a recipe: user recipes (stage 4B) override builtin ones —
// it is the user's machine and __recipe-list shows the source.

#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "recipes.h"

namespace berth {

[[nodiscard]] Result<std::vector<Recipe>> builtin_catalog();

[[nodiscard]] Result<Recipe> builtin_recipe(std::string_view service_name);

[[nodiscard]] Result<Recipe> resolve_recipe(const Paths& paths,
                                            std::string_view service_name);

[[nodiscard]] std::string render_catalog_list();

// The `--json` document for the builtin catalog (plan 5.3): versioned
// envelope plus a `recipes` array sorted by service_name.
[[nodiscard]] nlohmann::json catalog_json();

}  // namespace berth