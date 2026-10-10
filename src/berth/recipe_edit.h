#pragma once

// src/berth/recipe_edit.h — the editing engine for an EXISTING user recipe
// (dev.md §8): fix a wrong start_cmd/port/limit without hand-editing JSON.
// The CLI (`__recipe-edit`) and the later TUI screen drive the SAME
// functions, because two edit paths drift (§11).
//
// Only the user-editable VALUES are here. Identity/pin fields (owner, name,
// commit, service_name, language, binary_name, tools, prebuilt, build_steps)
// are deliberately NOT editable: changing those is a re-pin/upgrade, a
// different act with its own confirmation. An edit that fails the ONE
// validator (validate_recipe) is a loud Err naming the violated rule and is
// never stored — storing a wrong edit is the §8/§11 trap this module exists
// to prevent.

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

// Exactly the user-editable values. An absent optional leaves the base
// recipe's value untouched; the engine never invents a value.
struct RecipeEdits {
  std::optional<std::string> start_cmd;
  std::optional<std::string> health_path;
  std::optional<std::string> display_name;
  std::optional<std::string> description;
  std::optional<std::uint16_t> port;
  std::optional<std::string> memory_max;
  std::optional<std::uint32_t> tasks_max;
};

// PURE: applies the set fields, then runs validate_recipe(). An invalid edit
// (start_cmd losing the loopback bind, empty limits) is an Err naming the
// rule; an edit with no fields set is an Err (silence is not a policy, §11).
[[nodiscard]] Result<Recipe> apply_recipe_edits(const Recipe& base,
                                                const RecipeEdits& edits);

// The one atomic update: the before image (to report what changed), the
// after image and the stored path. Returning all three avoids a second,
// racy read and a second path derivation (the ONE writer is
// user_recipe_store; §11).
struct RecipeEditResult {
  Recipe before;
  Recipe after;
  std::filesystem::path stored_path;
};

// Loads the EXISTING USER recipe (user_recipe_get), applies the edits and
// stores through the ONE writer user_recipe_store(..., force=true). A name
// that is only in the builtin catalog — or absent everywhere — is an Err
// telling the user to create a user override first (__recipe-add --force or
// the wizard), never a silent builtin edit.
[[nodiscard]] Result<RecipeEditResult> user_recipe_update(
    const Paths& paths, std::string_view service_name,
    const RecipeEdits& edits);

// PURE CLI parser: the tokens AFTER the service name, e.g.
// {"--start-cmd", "{self} --addr 127.0.0.1 --port {port}"}. Requires at
// least one edit flag; an unknown flag or a malformed value is an Err.
[[nodiscard]] Result<RecipeEdits> parse_recipe_edit_args(
    const std::vector<std::string>& tokens);

// PURE: the "field: old -> new" report for what actually changed, or
// "(no field changed)" when every set value equalled the existing one.
[[nodiscard]] std::string render_recipe_edit_diff(const Recipe& before,
                                                  const Recipe& after);

}  // namespace berth
