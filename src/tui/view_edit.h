#pragma once

// This file only validates and shapes the form data; the apply + store is
// berth/recipe_edit.cpp, the ONE engine the CLI (`__recipe-edit`) also drives
// (§11: two edit paths drift).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../berth/recipe_edit.h"
#include "../berth/recipes.h"
#include "../util/result.h"
#include "chrome.h"  // KeyHint: the shared keys+words structured hint
#include "view_catalog.h"

namespace tui {

// kNone means "insert mode off" (§12.16): the screen's own keys (q, s, Esc)
// work and 'i' enters. A separate enum from CreateField only because the two
// field SETS differ — the same focus model on different data.
enum class EditField {
  kNone,
  kStartCmd,
  kBind,
  kHealthPath,
  kDisplayName,
  kDescription,
  kPort,
  kMemoryMax,
  kTasksMax,
};

inline constexpr std::size_t kEditFieldCount = 8;

[[nodiscard]] std::string_view edit_field_label(EditField field);

[[nodiscard]] EditField next_edit_field(EditField field);

// Out-of-range index is an invariant violation, not an Err.
[[nodiscard]] EditField edit_field_at(std::size_t index);

// port/tasks stay text until save: a bad number is refused by
// assemble_recipe_edits(), never dropped as it is typed (§11).
struct EditFields {
  std::string start_cmd;
  std::string bind;
  std::string health_path;
  std::string display_name;
  std::string description;
  std::string port;
  std::string memory_max;
  std::string tasks_max;
};

// Port 0 (free at deploy time) renders as an empty field, the same "leave it
// empty for a free port" the deploy screen documents.
[[nodiscard]] EditFields edit_fields_from_recipe(const berth::Recipe& recipe);

// The form is seeded, so every field is set; a bad start_cmd or a lost limit is
// refused later by apply_recipe_edits(), the ONE validator.
[[nodiscard]] berth::Result<berth::RecipeEdits> assemble_recipe_edits(
    const EditFields& fields);

[[nodiscard]] std::string edit_refusal_message(RecipeSource source);

[[nodiscard]] std::vector<std::string> edit_saved_lines(
    std::string_view diff, const std::filesystem::path& stored_path);

// `running` locks the fields while the write is in flight.
[[nodiscard]] std::vector<KeyHint> edit_key_hints(EditField focus,
                                                  bool running);

}
