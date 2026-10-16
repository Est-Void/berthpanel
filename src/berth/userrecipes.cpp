#include "userrecipes.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "../core/atomic.h"
#include "../core/json.h"
#include "../core/jsonout.h"
#include "../core/names.h"
#include "../util/fs.h"
#include "catalog.h"

namespace berth {

namespace {

// The prefix is forced, not demanded, so "myapp" lands as "berthpanel-myapp".
Result<std::string> normalize_service_name(const std::string& service_name) {
  const std::string prefixed = force_unit_prefix(service_name);
  if (!validate_unit_name(prefixed)) {
    return Error("userrecipes: service_name '" + prefixed +
                 "' does not pass validate_unit_name (must be '" +
                 std::string(kUnitPrefix) +
                 "…' with only [A-Za-z0-9._@-], max 128)");
  }
  return prefixed;
}

std::filesystem::path recipe_file(const Paths& paths,
                                  const std::string& service_name) {
  return paths.recipes_dir / (service_name + ".json");
}

}
Result<std::filesystem::path> user_recipe_store(const Paths& paths,
                                                const Recipe& recipe,
                                                const bool force) {
  Result<std::string> name = normalize_service_name(recipe.service_name);
  if (name.is_err())
    return std::move(name).error();

  Recipe stored = recipe;
  stored.service_name = name.value();
  // The ONE validator, at store time: an invalid recipe never reaches the disk.
  Result<void> valid = validate_recipe(stored);
  if (valid.is_err()) {
    return Error("userrecipes: recipe '" + name.value() +
                 "' rejected: " + valid.error().ctx());
  }

  // Replacing or shadowing a builtin hides what deploys: both need `force`.
  if (!force) {
    const Result<std::optional<Recipe>> existing =
        user_recipe_get(paths, name.value());
    if (existing.is_err())
      return std::move(existing).error();
    if (existing.value().has_value()) {
      return Error("userrecipes: '" + name.value() +
                   "' is already stored as a user recipe; pass --force to "
                   "overwrite it");
    }
    const Result<Recipe> builtin = builtin_recipe(name.value());
    if (builtin.is_ok()) {
      return Error("userrecipes: '" + name.value() +
                   "' collides with the builtin catalog; pass --force to "
                   "store an override");
    }
  }

  Result<void> dir = ensure_dir(paths.recipes_dir);
  if (dir.is_err())
    return std::move(dir).error();
  const std::filesystem::path target = recipe_file(paths, name.value());
  // 0644: env KEYS only, never values.
  Result<void> written =
      atomic_write(target, recipe_to_json(stored).dump(2) + "\n", kPermShared);
  if (written.is_err())
    return std::move(written).error();
  return target;
}

namespace {

Result<std::vector<std::filesystem::path>> recipe_files(const Paths& paths) {
  std::error_code ec;
  if (!std::filesystem::exists(paths.recipes_dir, ec)) {
    if (ec) {
      return Error("userrecipes: stat " + paths.recipes_dir.string() + ": " +
                   ec.message());
    }
    return std::vector<std::filesystem::path>{};  // nothing stored yet
  }
  std::vector<std::filesystem::path> files;
  for (const std::filesystem::directory_entry& item :
       std::filesystem::directory_iterator(paths.recipes_dir, ec)) {
    if (ec)
      break;
    if (!item.is_regular_file(ec) || ec)
      continue;
    if (item.path().extension() != ".json")
      continue;
    files.push_back(item.path());
  }
  if (ec) {
    return Error("userrecipes: scan " + paths.recipes_dir.string() + ": " +
                 ec.message());
  }
  std::sort(files.begin(), files.end());
  return files;
}

// One corrupt file is loud: json_load already quarantined it, name WHICH.
Result<Recipe> load_validated(const std::filesystem::path& file) {
  Result<nlohmann::json> loaded = json_load(file);
  if (loaded.is_err()) {
    return Error("userrecipes: " + file.string() + ": " + loaded.error().ctx());
  }
  Result<Recipe> parsed = recipe_from_json(loaded.value());
  if (parsed.is_err()) {
    return Error("userrecipes: " + file.string() + ": " + parsed.error().ctx());
  }
  Result<void> valid = validate_recipe(parsed.value());
  if (valid.is_err()) {
    return Error("userrecipes: " + file.string() +
                 " failed validation: " + valid.error().ctx());
  }
  // A renamed hand-edit must not store a recipe its file cannot find again.
  if (file.filename().replace_extension() != parsed.value().service_name) {
    return Error("userrecipes: " + file.string() + ": service_name '" +
                 parsed.value().service_name +
                 "' does not match the file name");
  }
  return std::move(parsed).value();
}

}

Result<std::vector<Recipe>> user_recipes_load(const Paths& paths) {
  const Result<std::vector<std::filesystem::path>> files = recipe_files(paths);
  if (files.is_err())
    return std::move(files).error();
  std::vector<Recipe> recipes;
  for (const std::filesystem::path& file : files.value()) {
    Result<Recipe> parsed = load_validated(file);
    if (parsed.is_err())
      return std::move(parsed).error();
    recipes.push_back(std::move(parsed).value());
  }
  return recipes;
}

Result<std::optional<Recipe>> user_recipe_get(
    const Paths& paths, const std::string_view service_name) {
  Result<std::vector<Recipe>> loaded = user_recipes_load(paths);
  if (loaded.is_err())
    return std::move(loaded).error();
  std::vector<Recipe> recipes = std::move(loaded).value();
  for (Recipe& recipe : recipes) {
    if (recipe.service_name == service_name) {
      return std::optional<Recipe>(std::move(recipe));
    }
  }
  return std::optional<Recipe>{};
}

Result<bool> user_recipe_remove(const Paths& paths,
                                const std::string_view service_name) {
  // The remover runs the SAME validator the creator ran (§11).
  if (!validate_unit_name(service_name)) {
    return Error("userrecipes: invalid service name: '" +
                 std::string(service_name) + "'");
  }
  std::error_code ec;
  const std::filesystem::path target =
      recipe_file(paths, std::string(service_name));
  const bool existed = std::filesystem::exists(target, ec);
  if (ec) {
    return Error("userrecipes: stat " + target.string() + ": " + ec.message());
  }
  if (!existed)
    return false;
  std::filesystem::remove(target, ec);
  if (ec) {
    return Error("userrecipes: remove " + target.string() + ": " +
                 ec.message());
  }
  return true;
}

std::string render_user_recipes_list(const std::vector<Recipe>& recipes) {
  if (recipes.empty()) {
    return "no user recipes stored\n";
  }
  std::string out;
  for (const Recipe& recipe : recipes) {
    out += "  " + recipe.service_name + "  ";
    out += recipe.owner_repo() + "@" + recipe.commit.substr(0, 7) + "  ";
    out += recipe.language + "  ";
    out += recipe.res.memory_max + "/" + std::to_string(recipe.res.tasks_max);
    out += "\n";
  }
  return out;
}

nlohmann::json user_recipes_json(const std::vector<Recipe>& recipes) {
  std::vector<Recipe> sorted = recipes;
  std::sort(sorted.begin(), sorted.end(), [](const Recipe& a, const Recipe& b) {
    return a.service_name < b.service_name;
  });
  nlohmann::json array = nlohmann::json::array();
  for (const Recipe& recipe : sorted) array.push_back(recipe_to_json(recipe));
  nlohmann::json doc = json_document("recipe-list");
  doc["recipes"] = std::move(array);
  return doc;
}

}
