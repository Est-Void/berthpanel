#pragma once

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
  kUser,
};

// A builtin is the frozen starter set: there is nothing to remove, so it is
// refused with a message, never a silent no-op.
struct RecipeRemoval {
  bool removable = false;
  std::string refusal;
};
[[nodiscard]] RecipeRemoval recipe_removal(RecipeSource source);

// The dialog DECIDES, the caller acts: "eaten" with nothing behind it is a
// dialog that lies (§11).
enum class RemoveConfirm {
  kConfirmed,  // y: remove the selected user recipe file
  kDismissed,  // n / Esc: keep it
  kQuit,       // q: dismiss and quit the panel
  kEaten,      // anything else: the modal keeps the key
};
[[nodiscard]] RemoveConfirm recipe_remove_key(std::string_view key);

// Names the file it deletes AND says a deployed service is a different thing
// (§11).
[[nodiscard]] std::vector<std::string> recipe_remove_confirm_lines(
    const std::string& service_name, const std::string& recipe_path);

struct Catalog {
  std::vector<berth::Recipe> recipes;
  std::vector<RecipeSource> sources;  // parallel to recipes
};

[[nodiscard]] berth::Result<Catalog> load_catalog(const berth::Paths& paths);

[[nodiscard]] std::string_view recipe_source_label(RecipeSource source);

// The header line is NOT selectable: the cursor indexes `recipes`, so one
// vector for both would highlight the header row and deploy the wrong recipe.
[[nodiscard]] std::string catalog_header_line();
[[nodiscard]] std::vector<std::string> catalog_row_lines(
    const Catalog& catalog);

// Out-of-range index is an invariant violation, not an Err: the screen owns the
// cursor.
[[nodiscard]] std::vector<std::string> catalog_detail_lines(
    const Catalog& catalog, std::size_t index);

// Display text only; the argv stays the truth and nothing parses this back.
[[nodiscard]] std::string render_step(const std::vector<std::string>& step);

}
