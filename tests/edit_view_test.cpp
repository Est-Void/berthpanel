// Tests for the recipe-edit view model (stage 4.19+/4.21 TUI). The load-bearing
// pieces are pure: the insert-mode focus cycle, the loud numeric parse that
// assembles a RecipeEdits, and the builtin refusal. No terminal, no engine
// call, no file.

#include "tui/view_edit.h"

#include <cstdint>
#include <string>
#include <vector>

#include "berth/recipe_edit.h"
#include "berth/recipes.h"
#include "runner.h"

namespace {

berth::Recipe go_recipe() {
  berth::Recipe r;
  r.owner = "someone";
  r.name = "myapp";
  r.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  r.language = "go";
  r.service_name = "berthpanel-myapp";
  r.display_name = "My App";
  r.description = "BYO app";
  r.build_steps = {{"go", "build", "-o", "{project}/myapp", "."}};
  r.binary_name = "myapp";
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port}";
  r.health_path = "/";
  r.tools = {"go"};
  r.res = {"384M", 200};
  return r;
}

bool contains(const std::vector<std::string>& haystack,
              const std::string_view needle) {
  for (const std::string& line : haystack) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(edit_focus_cycle_visits_every_field_then_wraps) {
  using tui::EditField;
  // kNone enters at the start command, like the wizard's kNone enters at url.
  CHECK(tui::next_edit_field(EditField::kNone) == EditField::kStartCmd);
  CHECK(tui::next_edit_field(EditField::kStartCmd) == EditField::kHealthPath);
  CHECK(tui::next_edit_field(EditField::kHealthPath) ==
        EditField::kDisplayName);
  CHECK(tui::next_edit_field(EditField::kDisplayName) ==
        EditField::kDescription);
  CHECK(tui::next_edit_field(EditField::kDescription) == EditField::kPort);
  CHECK(tui::next_edit_field(EditField::kPort) == EditField::kMemoryMax);
  CHECK(tui::next_edit_field(EditField::kMemoryMax) == EditField::kTasksMax);
  // The cycle wraps back to the first field, never to kNone.
  CHECK(tui::next_edit_field(EditField::kTasksMax) == EditField::kStartCmd);
}

TEST(edit_field_labels_match_the_slots) {
  CHECK(tui::edit_field_label(tui::EditField::kStartCmd) == "start_cmd");
  CHECK(tui::edit_field_label(tui::EditField::kHealthPath) == "health_path");
  CHECK(tui::edit_field_label(tui::EditField::kDisplayName) == "display_name");
  CHECK(tui::edit_field_label(tui::EditField::kDescription) == "description");
  CHECK(tui::edit_field_label(tui::EditField::kPort) == "port");
  CHECK(tui::edit_field_label(tui::EditField::kMemoryMax) == "memory_max");
  CHECK(tui::edit_field_label(tui::EditField::kTasksMax) == "tasks_max");
  CHECK(tui::edit_field_label(tui::EditField::kNone).empty());
}

TEST(edit_field_at_matches_the_input_slot_order) {
  // The renderer's input vector is in this order, so an off-by-one would draw
  // one field's value under another field's label.
  CHECK(tui::edit_field_at(0) == tui::EditField::kStartCmd);
  CHECK(tui::edit_field_at(1) == tui::EditField::kHealthPath);
  CHECK(tui::edit_field_at(2) == tui::EditField::kDisplayName);
  CHECK(tui::edit_field_at(3) == tui::EditField::kDescription);
  CHECK(tui::edit_field_at(4) == tui::EditField::kPort);
  CHECK(tui::edit_field_at(5) == tui::EditField::kMemoryMax);
  CHECK(tui::edit_field_at(6) == tui::EditField::kTasksMax);
}

TEST(edit_fields_seed_from_the_recipe) {
  const berth::Recipe recipe = go_recipe();
  const tui::EditFields fields = tui::edit_fields_from_recipe(recipe);
  CHECK(fields.start_cmd == recipe.start_cmd);
  CHECK(fields.health_path == recipe.health_path);
  CHECK(fields.display_name == recipe.display_name);
  CHECK(fields.description == recipe.description);
  CHECK(fields.memory_max == "384M");
  CHECK(fields.tasks_max == "200");
  // Port 0 means "free at deploy time": the field is empty, not the literal 0.
  CHECK(fields.port.empty());

  berth::Recipe fixed = recipe;
  fixed.port = 8123;
  CHECK(tui::edit_fields_from_recipe(fixed).port == "8123");
}

TEST(edit_assembles_recipe_edits_from_the_form) {
  tui::EditFields fields = tui::edit_fields_from_recipe(go_recipe());
  fields.start_cmd = "{self} --addr 127.0.0.1 --port {port} --fixed";
  fields.port = "8123";
  fields.memory_max = "768M";
  fields.tasks_max = "400";
  fields.display_name = "Fixed App";

  const berth::Result<berth::RecipeEdits> edits =
      tui::assemble_recipe_edits(fields);
  CHECK(edits.is_ok());
  CHECK(edits.value().start_cmd.value() == fields.start_cmd);
  CHECK(edits.value().port.value() == 8123);
  CHECK(edits.value().memory_max.value() == "768M");
  CHECK(edits.value().tasks_max.value() == 400);
  CHECK(edits.value().display_name.value() == "Fixed App");
  // Every seeded field is set, so the ONE validator sees the whole form.
  CHECK(edits.value().health_path.has_value());
  CHECK(edits.value().description.has_value());
}

TEST(edit_empty_port_means_free) {
  tui::EditFields fields = tui::edit_fields_from_recipe(go_recipe());
  fields.port = "";
  const berth::Result<berth::RecipeEdits> edits =
      tui::assemble_recipe_edits(fields);
  CHECK(edits.is_ok());
  CHECK(edits.value().port.value() == 0);
}

TEST(edit_rejects_a_non_numeric_port_loudly) {
  tui::EditFields fields = tui::edit_fields_from_recipe(go_recipe());

  fields.port = "80O";  // a letter, not a zero
  const berth::Result<berth::RecipeEdits> bad =
      tui::assemble_recipe_edits(fields);
  CHECK(bad.is_err());
  CHECK(bad.error().ctx().find("port") != std::string::npos);

  fields.port = "70000";
  CHECK(tui::assemble_recipe_edits(fields).is_err());

  // 0 typed is refused by the shared parser; the way to ask for a free port is
  // to leave the field empty.
  fields.port = "0";
  CHECK(tui::assemble_recipe_edits(fields).is_err());
}

TEST(edit_rejects_a_non_numeric_tasks_loudly) {
  tui::EditFields fields = tui::edit_fields_from_recipe(go_recipe());

  fields.tasks_max = "20O";
  const berth::Result<berth::RecipeEdits> bad =
      tui::assemble_recipe_edits(fields);
  CHECK(bad.is_err());
  CHECK(bad.error().ctx().find("tasks_max") != std::string::npos);

  fields.tasks_max = "0";
  CHECK(tui::assemble_recipe_edits(fields).is_err());

  fields.tasks_max = "";
  CHECK(tui::assemble_recipe_edits(fields).is_err());
}

TEST(edit_an_invalid_start_cmd_surfaces_the_validator_error) {
  // The assembled edits reach the SAME engine the CLI uses. A start_cmd that
  // drops the loopback bind must be refused there, naming the rule — the
  // §8/§11 trap of storing a wrong edit.
  tui::EditFields fields = tui::edit_fields_from_recipe(go_recipe());
  fields.start_cmd = "{self} --port {port}";
  const berth::Result<berth::RecipeEdits> edits =
      tui::assemble_recipe_edits(fields);
  CHECK(edits.is_ok());  // the form itself is well-formed
  const berth::Result<berth::Recipe> applied =
      berth::apply_recipe_edits(go_recipe(), edits.value());
  CHECK(applied.is_err());
  CHECK(applied.error().ctx().find("loopback") != std::string::npos);
}

TEST(edit_refuses_a_builtin_recipe_but_not_a_user_one) {
  const std::string refusal =
      tui::edit_refusal_message(tui::RecipeSource::kBuiltin);
  CHECK(refusal.find("builtin") != std::string::npos);
  // The way forward is named, not just the refusal.
  CHECK(refusal.find("user override") != std::string::npos);
  CHECK(refusal.find("__recipe-add") != std::string::npos);

  CHECK(tui::edit_refusal_message(tui::RecipeSource::kUser).empty());
}

TEST(edit_saved_lines_name_the_file_and_the_change) {
  berth::Recipe before = go_recipe();
  berth::Recipe after = before;
  after.start_cmd = "{self} --addr 127.0.0.1 --port {port} --fixed";
  after.port = 8123;
  const std::string diff = berth::render_recipe_edit_diff(before, after);

  const std::vector<std::string> lines = tui::edit_saved_lines(
      diff, "/home/u/.config/berthpanel/recipes/berthpanel-myapp.json");
  CHECK(contains(lines, "berthpanel-myapp.json"));
  CHECK(contains(lines, "start_cmd:"));
  CHECK(contains(lines, "port: 0 -> 8123"));
}

TEST(edit_key_hints_follow_the_state_they_are_in) {
  // The form with no field focused: enter the fields, save, or leave.
  const std::vector<std::string> idle =
      tui::edit_key_hints(tui::EditField::kNone, false);
  CHECK(contains(idle, "i  edit the fields"));
  CHECK(contains(idle, "s  save the edit"));
  CHECK(contains(idle, "Esc  leave the edit screen"));

  // A field focused: Tab cycles, Esc releases.
  const std::vector<std::string> typing =
      tui::edit_key_hints(tui::EditField::kStartCmd, false);
  CHECK(contains(typing, "Tab  next field"));
  CHECK(contains(typing, "Esc  release the field"));

  // In flight: the fields are locked and only quitting is on offer.
  const std::vector<std::string> running =
      tui::edit_key_hints(tui::EditField::kNone, true);
  CHECK(contains(running, "being written"));
  CHECK(!contains(running, "s  save"));
}
