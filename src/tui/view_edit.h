#pragma once

// src/tui/view_edit.h — the recipe-edit view model (dev.md §8, stage
// 4.19+/4.21 TUI): the eight user-editable fields, the insert-mode focus
// cycle, the loud numeric parsing that assembles a RecipeEdits, and the
// builtin refusal. Pure and FTXUI-free: the screen in screens.cpp builds
// Elements from these strings and values, and the tests assert them without a
// terminal.
//
// The edit itself is NOT here: this file only validates and shapes the form
// data. The actual apply + store is berth/recipe_edit.cpp, the ONE engine the
// CLI (`__recipe-edit`) also drives (§11: two edit paths drift).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../berth/recipe_edit.h"
#include "../berth/recipes.h"
#include "../util/result.h"
#include "view_catalog.h"

namespace tui {

// Which editable field has focus. kNone is the default and means "insert mode
// off", exactly like DraftField (§12.16): while no field is focused the
// screen's own keys (q, s, Esc) work, and 'i' enters the fields. A separate
// enum from DraftField because the two field SETS differ — not a second focus
// model, the same one applied to different data.
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

// The eight selectable slots, in focus order.
inline constexpr std::size_t kEditFieldCount = 8;

[[nodiscard]] std::string_view edit_field_label(EditField field);

// The next field in the cycle: start_cmd → bind → health_path → display_name →
// description → port → memory_max → tasks_max → start_cmd. kNone enters at
// start_cmd.
[[nodiscard]] EditField next_edit_field(EditField field);

// The field at an input-slot index (0..kEditFieldCount-1), for the renderer's
// loop. An out-of-range index is an invariant violation and aborts.
[[nodiscard]] EditField edit_field_at(std::size_t index);

// The eight values as typed. port/tasks stay text until save, so a bad number
// is refused by assemble_recipe_edits() rather than silently dropped as it is
// typed (§11).
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

// Seeds the form from the base recipe. Port 0 (free at deploy time) renders as
// an empty field, which is the same "leave it empty for a free port" the
// deploy screen documents.
[[nodiscard]] EditFields edit_fields_from_recipe(const berth::Recipe& recipe);

// PURE: assembles a RecipeEdits from the typed form, parsing port and tasks
// LOUDLY. Every field is set (the form is seeded), so an invalid start_cmd or
// a lost limit is refused later by the ONE validator in apply_recipe_edits().
[[nodiscard]] berth::Result<berth::RecipeEdits> assemble_recipe_edits(
    const EditFields& fields);

// A builtin recipe has no user file to edit: the action is refused, with the
// way forward named. Empty when the source IS editable.
[[nodiscard]] std::string edit_refusal_message(RecipeSource source);

// After a successful save: where the recipe now lives and what changed, from
// berth::render_recipe_edit_diff(). One line per change.
[[nodiscard]] std::vector<std::string> edit_saved_lines(
    std::string_view diff, const std::filesystem::path& stored_path);

// The keys this screen acts on right now, one line per group. `running` locks
// the fields while the write is in flight.
[[nodiscard]] std::vector<std::string> edit_key_hints(EditField focus,
                                                      bool running);

}  // namespace tui
