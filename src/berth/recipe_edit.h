#pragma once

// Editing engine for an EXISTING user recipe: the CLI and the TUI drive the
// SAME functions, because two edit paths drift (§11). Identity/pin fields are
// not editable — a re-pin. An edit that fails validate_recipe is never stored.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "recipes.h"

namespace berth {

// An absent optional leaves the base value untouched.
struct RecipeEdits {
  std::optional<std::string> start_cmd;
  std::optional<std::string> bind;
  std::optional<std::string> health_path;
  std::optional<std::string> display_name;
  std::optional<std::string> description;
  std::optional<std::uint16_t> port;
  std::optional<std::string> memory_max;
  std::optional<std::uint32_t> tasks_max;
};

// PURE: applies the set fields, then validate_recipe(). No field set is an Err.
[[nodiscard]] Result<Recipe> apply_recipe_edits(const Recipe& base,
                                                const RecipeEdits& edits);

// One return, no second racy read; user_recipe_store is the ONE writer (§11).
struct RecipeEditResult {
  Recipe before;
  Recipe after;
  std::filesystem::path stored_path;
};

// Loads the EXISTING USER recipe, stores through user_recipe_store(force=true).
[[nodiscard]] Result<RecipeEditResult> user_recipe_update(
    const Paths& paths, std::string_view service_name,
    const RecipeEdits& edits);

[[nodiscard]] Result<RecipeEdits> parse_recipe_edit_args(
    const std::vector<std::string>& tokens);

[[nodiscard]] std::string render_recipe_edit_diff(const Recipe& before,
                                                  const Recipe& after);

}
