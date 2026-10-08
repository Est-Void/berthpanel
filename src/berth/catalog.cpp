#include "catalog.h"

#include <algorithm>
#include <string>

#include "../core/jsonout.h"
#include "../core/names.h"
#include "userrecipes.h"
#include "validate.h"

namespace berth {

namespace {

// Memos 0.31.0 ships prebuilt release assets with published checksums, so
// the deploy needs no compiler on the host: a pinned tarball verified by
// SHA-256 (toolbox) instead of a bun+go build (dev.md §8 "prebuilt binary
// through toolbox"). The commit is the tag's own SHA — a branch would be
// exactly the anti-TOFU violation §6.6 forbids.
Recipe memos_recipe() {
  Recipe r;
  r.owner = "usememos";
  r.name = "memos";
  r.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  r.language = "prebuilt";
  r.service_name = force_unit_prefix("memos");
  r.display_name = "Memos";
  r.description = "Self-hosted note taking";
  r.prebuilt = PrebuiltBinary{
      .url =
          "https://github.com/usememos/memos/releases/download/v0.31.0/"
          "memos_0.31.0_linux_amd64.tar.gz",
      .sha256 =
          "d99bf9de5e947cd41f7f1ae59e1e97d9af933d1bcc2d1316b3ab1ffe0a69e5c0",
      .member = "memos"};
  r.binary_name = "memos";
  // --addr 127.0.0.1 is the loopback bind the catalog contract demands;
  // memos would otherwise listen on every interface.
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port} --data {project}/data";
  r.health_path = "/";
  r.port = 0;
  r.tools = {};
  r.res = ResourceLimits{.memory_max = "512M", .tasks_max = 300};
  return r;
}

}  // namespace

Result<std::vector<Recipe>> builtin_catalog() {
  std::vector<Recipe> catalog = {memos_recipe()};
  for (const Recipe& recipe : catalog) {
    const Result<void> valid = validate_recipe(recipe);
    if (valid.is_err())
      return std::move(valid).error();
  }
  return catalog;
}

Result<Recipe> builtin_recipe(const std::string_view service_name) {
  Result<std::vector<Recipe>> catalog = builtin_catalog();
  if (catalog.is_err())
    return std::move(catalog).error();
  for (const Recipe& recipe : catalog.value()) {
    if (recipe.service_name == service_name)
      return recipe;
  }
  return Error("catalog: no builtin recipe named '" +
               std::string(service_name) + "'");
}

Result<Recipe> resolve_recipe(const Paths& paths,
                              const std::string_view service_name) {
  // A user recipe with the same name WINS: the stored file is the
  // explicit act of replacing the curated default. Loading user recipes
  // is loud — one corrupt file must not read as "no recipes" (§11).
  Result<std::optional<Recipe>> user = user_recipe_get(paths, service_name);
  if (user.is_err())
    return std::move(user).error();
  if (user.value().has_value())
    return user.value().value();
  return builtin_recipe(service_name);
}

std::string render_catalog_list() {
  Result<std::vector<Recipe>> catalog = builtin_catalog();
  if (catalog.is_err())
    return "catalog: error: " + catalog.error().ctx() + "\n";
  std::string out;
  for (const Recipe& recipe : catalog.value()) {
    out += "  " + recipe.service_name + "  ";
    out += recipe.display_name;
    out += "  (" + recipe.language + ", ";
    out += recipe.port == 0 ? "random port" : std::to_string(recipe.port);
    out += ", " + recipe.res.memory_max + "/" +
           std::to_string(recipe.res.tasks_max) + ")\n";
  }
  return out;
}

nlohmann::json catalog_json() {
  Result<std::vector<Recipe>> catalog = builtin_catalog();
  nlohmann::json doc = json_document("catalog");
  if (catalog.is_err()) {
    doc["error"] = catalog.error().ctx();
    return doc;
  }
  std::vector<Recipe> sorted = std::move(catalog).value();
  std::sort(sorted.begin(), sorted.end(), [](const Recipe& a, const Recipe& b) {
    return a.service_name < b.service_name;
  });
  nlohmann::json array = nlohmann::json::array();
  for (const Recipe& recipe : sorted) array.push_back(recipe_to_json(recipe));
  doc["recipes"] = std::move(array);
  return doc;
}

}  // namespace berth