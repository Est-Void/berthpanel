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
#include <string_view>
#include <vector>

#include "../berth/recipes.h"
#include "../core/paths.h"
#include "../util/result.h"

namespace tui {

enum class RecipeSource {
  kBuiltin,
  kUser,  // a user recipe, which OVERRIDES a builtin of the same name
};

// Whether the SELECTED recipe can be removed from the Deploy (catalog) list,
// and why not when it cannot. A user recipe is a file the user owns, so it is
// removable; a builtin is the frozen starter set — there is nothing to remove
// and it is refused with a clear message (never a silent no-op). Pure, so the
// builtin/user decision is tested without a terminal.
struct RecipeRemoval {
  bool removable = false;
  std::string refusal;  // empty when removable
};
[[nodiscard]] RecipeRemoval recipe_removal(RecipeSource source);

// What a key press means while the removal confirmation is up. The dialog
// DECIDES; the caller (app.cpp) acts — a confirmation that returns "eaten"
// without a removal behind it is a dialog that lies (§11).
enum class RemoveConfirm {
  kConfirmed,  // y: remove the selected user recipe file
  kDismissed,  // n / Esc: keep it
  kQuit,       // q: dismiss and quit the panel
  kEaten,      // anything else: the modal keeps the key
};
[[nodiscard]] RemoveConfirm recipe_remove_key(std::string_view key);

// The removal confirmation. It names the recipe FILE being deleted (the real
// path, passed in — never a hardcoded XDG default) AND says that a DEPLOYED
// service is a different thing, uninstalled on the My Services tab: a
// confirmation that hides what it deletes is not a confirmation (§11).
[[nodiscard]] std::vector<std::string> recipe_remove_confirm_lines(
    const std::string& service_name, const std::string& recipe_path);

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