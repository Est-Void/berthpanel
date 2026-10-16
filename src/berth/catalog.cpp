#include "catalog.h"

#include <algorithm>
#include <string>

#include "../core/jsonout.h"
#include "../core/names.h"
#include "userrecipes.h"
#include "validate.h"

namespace berth {

namespace {

// Commit is the tag's own SHA; a branch would be the anti-TOFU violation §6.6.
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
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port} --data {project}/data";
  r.health_path = "/";
  r.port = 0;
  r.tools = {};
  r.res = ResourceLimits{.memory_max = "512M", .tasks_max = 300};
  return r;
}

Recipe filebrowser_recipe() {
  Recipe r;
  r.owner = "filebrowser";
  r.name = "filebrowser";
  r.commit = "e8a388f840173580116f2743813d03b22286e44e";
  r.language = "prebuilt";
  r.service_name = force_unit_prefix("filebrowser");
  r.display_name = "Filebrowser";
  r.description = "Self-hosted file management web UI";
  r.prebuilt = PrebuiltBinary{
      .url =
          "https://github.com/filebrowser/filebrowser/releases/download/"
          "v2.63.23/linux-amd64-filebrowser.tar.gz",
      .sha256 =
          "b14db2bb8033caa3f80205eb6578b2ed0744ebd9e716b790bc4a9703ce909e88",
      .member = "filebrowser"};
  r.binary_name = "filebrowser";
  // --database/--root must sit inside {project}: the defaults hit install_dir.
  r.start_cmd =
      "{self} --address 127.0.0.1 --port {port} --database {project}/"
      "filebrowser.db --root {project}/files";
  r.health_path = "/";
  r.port = 0;
  r.tools = {};
  r.res = ResourceLimits{.memory_max = "256M", .tasks_max = 300};
  return r;
}

Recipe gitea_recipe() {
  Recipe r;
  r.owner = "go-gitea";
  r.name = "gitea";
  r.commit = "4ebd5e319b6a9e629bfd7a04292a6fe85b08dee9";
  r.language = "prebuilt";
  r.service_name = force_unit_prefix("gitea");
  r.display_name = "Gitea";
  r.description = "Self-hosted Git hosting service";
  r.prebuilt = PrebuiltBinary{
      .url =
          "https://github.com/go-gitea/gitea/releases/download/v28.1.0/"
          "gitea-28.1.0-linux-amd64",
      .sha256 =
          "ea93cfa4a300564ac457d3903469fea49ec5a4da37d0cebaa435f8e329ea64d0",
      .member = "gitea",
      .archive = ToolArchive::kRaw};
  r.binary_name = "gitea";
  // The bind comes from env; its default is the wildcard.
  r.start_cmd =
      "env GITEA__server__HTTP_ADDR=127.0.0.1 {self} web --port {port}";
  r.health_path = "/";
  r.port = 0;
  r.tools = {};
  r.res = ResourceLimits{.memory_max = "1024M", .tasks_max = 512};
  return r;
}

Recipe uptime_kuma_recipe() {
  Recipe r;
  r.owner = "louislam";
  r.name = "uptime-kuma";
  r.commit = "7ca4a3debd76443d8452da95d4040200a1479af4";
  // bun is a JS runtime; "node" is the supported language token for it.
  r.language = "node";
  r.service_name = force_unit_prefix("uptime-kuma");
  r.display_name = "Uptime Kuma";
  r.description = "Self-hosted uptime monitoring";
  r.prefetch_steps = {{"bun", "install", "--ignore-scripts"}};
  r.build_steps = {{"bun", "install"}, {"bun", "run", "build"}};
  r.binary_name = "server/server.js";
  // args-parser@1.3.0 needs --host=127.0.0.1; the space form is ignored.
  r.start_cmd = "bun {project}/server/server.js --host=127.0.0.1 --port={port}";
  r.health_path = "/";
  r.port = 0;
  r.tools = {"bun"};
  r.res = ResourceLimits{.memory_max = "768M", .tasks_max = 512};
  return r;
}

}
Result<std::vector<Recipe>> builtin_catalog() {
  std::vector<Recipe> catalog = {memos_recipe(), filebrowser_recipe(),
                                 gitea_recipe(), uptime_kuma_recipe()};
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

}
