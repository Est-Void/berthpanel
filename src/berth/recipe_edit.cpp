// src/berth/recipe_edit.cpp — see recipe_edit.h for the contract.

#include "recipe_edit.h"

#include <cstdint>
#include <string>
#include <utility>

#include "catalog.h"
#include "userrecipes.h"

namespace berth {

namespace {

// Strict decimal parse, shared by --port and --tasks. Deliberately not
// strtol: a trailing character ("80O" with a letter) must be an Err, not
// the silent prefix strtol would return (§11 silent wrong answer).
Result<std::uint64_t> parse_uint(const std::string& text,
                                 const std::string_view flag) {
  if (text.empty()) {
    return Error("recipe edit: " + std::string(flag) + " needs a number");
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return Error("recipe edit: " + std::string(flag) + " '" + text +
                   "' is not a number");
    }
    value = value * 10U + static_cast<std::uint64_t>(digit - '0');
    if (value > 0xFFFFFFFFULL) {
      return Error("recipe edit: " + std::string(flag) + " '" + text +
                   "' is too large");
    }
  }
  return value;
}

}  // namespace

Result<Recipe> apply_recipe_edits(const Recipe& base,
                                  const RecipeEdits& edits) {
  const bool any = edits.start_cmd.has_value() ||
                   edits.health_path.has_value() ||
                   edits.display_name.has_value() ||
                   edits.description.has_value() || edits.port.has_value() ||
                   edits.memory_max.has_value() || edits.tasks_max.has_value();
  if (!any) {
    return Error("recipe edit: no fields to apply");
  }
  Recipe edited = base;
  if (edits.start_cmd.has_value())
    edited.start_cmd = edits.start_cmd.value();
  if (edits.health_path.has_value())
    edited.health_path = edits.health_path.value();
  if (edits.display_name.has_value())
    edited.display_name = edits.display_name.value();
  if (edits.description.has_value())
    edited.description = edits.description.value();
  if (edits.port.has_value())
    edited.port = edits.port.value();
  if (edits.memory_max.has_value())
    edited.res.memory_max = edits.memory_max.value();
  if (edits.tasks_max.has_value())
    edited.res.tasks_max = edits.tasks_max.value();

  // The ONE validator, exactly as at store and load time. A rejected edit
  // never leaves this function, so it can never be stored (§8/§11).
  const Result<void> valid = validate_recipe(edited);
  if (valid.is_err()) {
    return Error("recipe edit rejected: " + valid.error().ctx());
  }
  return edited;
}

Result<RecipeEditResult> user_recipe_update(const Paths& paths,
                                            const std::string_view service_name,
                                            const RecipeEdits& edits) {
  Result<std::optional<Recipe>> got = user_recipe_get(paths, service_name);
  if (got.is_err()) {
    return with_context(std::move(got).error(), "recipe edit");
  }
  if (!got.value().has_value()) {
    // A builtin-only name is a different act: create a user override first.
    const Result<Recipe> builtin = builtin_recipe(service_name);
    if (builtin.is_ok()) {
      return Error(
          "recipe edit: '" + std::string(service_name) +
          "' is a builtin recipe, not a user recipe; create a user override "
          "first (berthpanel __recipe-add <file.json> --force, or the wizard "
          "__recipe-draft)");
    }
    return Error("recipe edit: no user recipe named '" +
                 std::string(service_name) + "'");
  }
  const Recipe base = std::move(got).value().value();

  Result<Recipe> edited = apply_recipe_edits(base, edits);
  if (edited.is_err()) {
    return std::move(edited).error();
  }

  // force=true: the recipe already exists by definition, and the edits were
  // just validated by apply_recipe_edits above; store re-validates and writes
  // atomically at 0644.
  Result<std::filesystem::path> stored =
      user_recipe_store(paths, edited.value(), true);
  if (stored.is_err()) {
    return with_context(std::move(stored).error(), "recipe edit");
  }
  return RecipeEditResult{base, std::move(edited).value(),
                          std::move(stored).value()};
}

Result<RecipeEdits> parse_recipe_edit_args(
    const std::vector<std::string>& tokens) {
  RecipeEdits edits;
  bool any = false;
  std::size_t index = 0;
  while (index < tokens.size()) {
    const std::string& flag = tokens[index];
    const bool has_value = index + 1 < tokens.size();
    const std::string& value = has_value ? tokens[index + 1] : flag;

    if (flag == "--start-cmd") {
      if (!has_value)
        return Error("recipe edit: --start-cmd needs a value");
      edits.start_cmd = value;
      any = true;
    } else if (flag == "--health") {
      if (!has_value)
        return Error("recipe edit: --health needs a path");
      edits.health_path = value;
      any = true;
    } else if (flag == "--name") {
      if (!has_value)
        return Error("recipe edit: --name needs a value");
      edits.display_name = value;
      any = true;
    } else if (flag == "--description") {
      if (!has_value)
        return Error("recipe edit: --description needs a value");
      edits.description = value;
      any = true;
    } else if (flag == "--memory") {
      if (!has_value)
        return Error("recipe edit: --memory needs a value (e.g. 512M)");
      edits.memory_max = value;
      any = true;
    } else if (flag == "--port") {
      if (!has_value)
        return Error("recipe edit: --port needs a value");
      const Result<std::uint64_t> number = parse_uint(value, "--port");
      if (number.is_err())
        return std::move(number).error();
      if (number.value() == 0 || number.value() > 65535ULL) {
        return Error("recipe edit: --port must be in 1-65535, got '" + value +
                     "'");
      }
      edits.port = static_cast<std::uint16_t>(number.value());
      any = true;
    } else if (flag == "--tasks") {
      if (!has_value)
        return Error("recipe edit: --tasks needs a value");
      const Result<std::uint64_t> number = parse_uint(value, "--tasks");
      if (number.is_err())
        return std::move(number).error();
      if (number.value() == 0) {
        return Error("recipe edit: --tasks must be at least 1");
      }
      edits.tasks_max = static_cast<std::uint32_t>(number.value());
      any = true;
    } else {
      return Error("recipe edit: unknown flag '" + flag + "'");
    }
    index += 2;
  }
  if (!any) {
    return Error(
        "recipe edit: at least one edit flag is required (--start-cmd, "
        "--port, --memory, --tasks, --health, --name, --description)");
  }
  return edits;
}

std::string render_recipe_edit_diff(const Recipe& before, const Recipe& after) {
  std::string out;
  const auto add = [&out](const std::string_view field,
                          const std::string& old_value,
                          const std::string& new_value) {
    if (old_value == new_value)
      return;
    out += "  ";
    out += field;
    out += ": ";
    out += old_value;
    out += " -> ";
    out += new_value;
    out += "\n";
  };
  add("start_cmd", before.start_cmd, after.start_cmd);
  add("health_path", before.health_path, after.health_path);
  add("display_name", before.display_name, after.display_name);
  add("description", before.description, after.description);
  add("port", std::to_string(before.port), std::to_string(after.port));
  add("res.memory_max", before.res.memory_max, after.res.memory_max);
  add("res.tasks_max", std::to_string(before.res.tasks_max),
      std::to_string(after.res.tasks_max));
  if (out.empty())
    out = "  (no field changed)\n";
  return out;
}

}  // namespace berth
