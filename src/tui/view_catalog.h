#pragma once

// src/tui/view_catalog.h — the Catalog view model (plan stage 6.2), as pure
// functions over data. No FTXUI, no threads: the screen builds Elements from
// these strings, and the tests assert these strings.
//
// Read-only by construction: nothing here deploys, writes a recipe or reaches
// the network. The catalog is two file reads (builtin_catalog +
// user_recipes_load) and the panel shows what it finds, including a recipe file
// that failed to validate — that load is a LOUD Err upstream and the screen
// shows the error text rather than an empty list (§11: a corrupt recipe must
// not silently disappear).

#include <cstdint>
#include <string>
#include <vector>

#include "../berth/recipes.h"
#include "../core/paths.h"
#include "../util/result.h"

namespace tui {

enum class RecipeSource {
  kBuiltin,
  kUser,  // a user recipe, which OVERRIDES a builtin of the same name
};

struct Catalog {
  std::vector<berth::Recipe> recipes;
  std::vector<RecipeSource> sources;  // parallel to recipes
};

// Both sources, in listing order: builtin first, then user recipes.
[[nodiscard]] berth::Result<Catalog> load_catalog(const berth::Paths& paths);

[[nodiscard]] std::string_view recipe_source_label(RecipeSource source);

// The table is a NON-SELECTABLE header line plus one selectable line per
// recipe, returned separately. They used to be one vector, and the screen fed
// the whole thing to line_list() with a cursor that indexes `recipes`: the
// highlight (and therefore Enter's target) was off by one row — the cursor
// sat on the header, and the recipe it deployed was never the one shown.
[[nodiscard]] std::string catalog_header_line();
[[nodiscard]] std::vector<std::string> catalog_row_lines(
    const Catalog& catalog);

// The details pane: description, tools, every argv step, the start command
// and the deploy hint. Out-of-range indices are an invariant violation and
// abort — the screen owns the cursor and cannot be out of range.
[[nodiscard]] std::vector<std::string> catalog_detail_lines(
    const Catalog& catalog, std::size_t index);

// The argv of a build step rendered for display. The argv itself stays the
// truth; this is text for a human, and nothing ever parses it back.
[[nodiscard]] std::string render_step(const std::vector<std::string>& step);

}  // namespace tui