// src/tui/view_edit.cpp — see view_edit.h for the contract.

#include "view_edit.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "../util/strutil.h"
#include "view_deploy.h"  // parse_port_field: the same loud port rule

namespace tui {

namespace {

[[noreturn]] void abort_out_of_range() {
  std::fputs("tui: invariant violated: edit field index out of range\n",
             stderr);
  std::abort();
}

// A tasks_max is a positive count: systemd's TasksMax has no zero and no
// negative form. Deliberately not strtol: a trailing character ("20O") must be
// an Err, never the silent prefix strtol would return (§11).
berth::Result<std::uint32_t> parse_tasks_field(const std::string_view field) {
  const std::string_view typed = berth::trim(field);
  if (typed.empty()) {
    return berth::Error("tasks_max: empty — a recipe must declare a limit §8");
  }
  std::uint64_t value = 0;
  for (const char digit : typed) {
    if (digit < '0' || digit > '9') {
      return berth::Error("tasks_max: '" + std::string(typed) +
                          "' is not a number — type a positive count");
    }
    value = value * 10U + static_cast<std::uint64_t>(digit - '0');
    if (value > 0xFFFFFFFFULL) {
      return berth::Error("tasks_max: '" + std::string(typed) +
                          "' is too large");
    }
  }
  if (value == 0) {
    return berth::Error(
        "tasks_max: 0 is not a limit — a recipe must declare a positive "
        "tasks_max (§8)");
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

std::string_view edit_field_label(const EditField field) {
  switch (field) {
    case EditField::kStartCmd:
      return "start_cmd";
    case EditField::kHealthPath:
      return "health_path";
    case EditField::kDisplayName:
      return "display_name";
    case EditField::kDescription:
      return "description";
    case EditField::kPort:
      return "port";
    case EditField::kMemoryMax:
      return "memory_max";
    case EditField::kTasksMax:
      return "tasks_max";
    case EditField::kNone:
      return "";
  }
  return "";
}

EditField next_edit_field(const EditField field) {
  switch (field) {
    case EditField::kNone:  // entering insert mode starts at the start command
    case EditField::kTasksMax:
      return EditField::kStartCmd;
    case EditField::kStartCmd:
      return EditField::kHealthPath;
    case EditField::kHealthPath:
      return EditField::kDisplayName;
    case EditField::kDisplayName:
      return EditField::kDescription;
    case EditField::kDescription:
      return EditField::kPort;
    case EditField::kPort:
      return EditField::kMemoryMax;
    case EditField::kMemoryMax:
      return EditField::kTasksMax;
  }
  return EditField::kStartCmd;
}

EditField edit_field_at(const std::size_t index) {
  switch (index) {
    case 0:
      return EditField::kStartCmd;
    case 1:
      return EditField::kHealthPath;
    case 2:
      return EditField::kDisplayName;
    case 3:
      return EditField::kDescription;
    case 4:
      return EditField::kPort;
    case 5:
      return EditField::kMemoryMax;
    case 6:
      return EditField::kTasksMax;
    default:
      abort_out_of_range();
  }
}

EditFields edit_fields_from_recipe(const berth::Recipe& recipe) {
  EditFields fields;
  fields.start_cmd = recipe.start_cmd;
  fields.health_path = recipe.health_path;
  fields.display_name = recipe.display_name;
  fields.description = recipe.description;
  fields.port = recipe.port == 0 ? std::string() : std::to_string(recipe.port);
  fields.memory_max = recipe.res.memory_max;
  fields.tasks_max = std::to_string(recipe.res.tasks_max);
  return fields;
}

berth::Result<berth::RecipeEdits> assemble_recipe_edits(
    const EditFields& fields) {
  berth::RecipeEdits edits;
  edits.start_cmd = fields.start_cmd;
  edits.health_path = fields.health_path;
  edits.display_name = fields.display_name;
  edits.description = fields.description;
  edits.memory_max = fields.memory_max;

  // The port reuses the deploy screen's parser verbatim: empty means "free at
  // deploy time" and anything that is not 1-65535 is refused by name. Two port
  // parsers that disagree is the §11 trap.
  berth::Result<std::uint16_t> port = parse_port_field(fields.port);
  if (port.is_err()) {
    return std::move(port).error();
  }
  edits.port = port.value();

  berth::Result<std::uint32_t> tasks = parse_tasks_field(fields.tasks_max);
  if (tasks.is_err()) {
    return std::move(tasks).error();
  }
  edits.tasks_max = tasks.value();
  return edits;
}

std::string edit_refusal_message(const RecipeSource source) {
  if (source == RecipeSource::kUser) {
    return {};
  }
  return "builtin recipe — create a user override first with the wizard "
         "(press i on the Deploy tab) or "
         "'berthpanel __recipe-add <file.json> --force'";
}

std::vector<std::string> edit_saved_lines(
    const std::string_view diff, const std::filesystem::path& stored_path) {
  std::vector<std::string> lines;
  lines.push_back("saved: " + stored_path.string());
  for (const std::string& line : berth::split(diff, "\n")) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

std::vector<std::string> edit_key_hints(const EditField focus,
                                        const bool running) {
  if (running) {
    return {"the edit is being written — wait for the verdict",
            "q  quit (the file write is atomic: no half-written recipe)"};
  }
  if (focus != EditField::kNone) {
    return {"Tab  next field", "Esc  release the field — s and q work again"};
  }
  return {"i  edit the fields", "s  save the edit (validated, then stored)",
          "Esc  leave the edit screen", "q  quit"};
}

}  // namespace tui
