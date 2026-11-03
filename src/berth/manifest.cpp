#include "manifest.h"

#include <string>
#include <utility>

#include "../core/jsonout.h"
#include "../util/fs.h"

namespace berth {

namespace {

std::string join_tools(const std::vector<std::string>& tools) {
  if (tools.empty()) {
    return "(none)";
  }
  std::string joined;
  for (const std::string& tool : tools) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += tool;
  }
  return joined;
}

std::string short_commit(const std::string& commit) {
  return commit.size() > 7 ? commit.substr(0, 7) : commit;
}

}

Result<Recipe> load_manifest(const std::filesystem::path& path) {
  const Result<std::string> text = read_file(path);
  if (text.is_err()) {
    return Error("manifest: " + path.string() + ": " + text.error().ctx());
  }
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(text.value());
  } catch (const nlohmann::json::parse_error& error) {
    return Error("manifest: " + path.string() +
                 ": not valid JSON: " + error.what());
  }
  try {
    Result<Recipe> recipe = recipe_from_json(doc);
    if (recipe.is_err()) {
      return Error("manifest: " + path.string() + ": " + recipe.error().ctx());
    }
    const Result<void> valid = validate_recipe(recipe.value());
    if (valid.is_err()) {
      return Error("manifest: " + path.string() + ": " + valid.error().ctx());
    }
    return std::move(recipe).value();
  } catch (const nlohmann::json::exception& error) {
    return Error("manifest: " + path.string() + ": invalid fields: " +
                 error.what());
  }
}

std::string render_manifest_plan(const Recipe& recipe) {
  std::string out = "manifest: ok\n";
  out += "  service:  " + recipe.service_name + "\n";
  out += "  repo:     " + recipe.owner_repo() + "@" + short_commit(recipe.commit) +
         "\n";
  out += "  language: " + recipe.language + "\n";
  out += "  start:    " + recipe.start_cmd + "\n";
  out += "  bind:     " +
         (recipe.bind.empty() ? std::string("(from start_cmd)") : recipe.bind) +
         "\n";
  out += "  egress:   " +
         (recipe.egress.empty() ? std::string("unrestricted") : recipe.egress) +
         "\n";
  out += "  limits:   " + recipe.res.memory_max + "/" +
         std::to_string(recipe.res.tasks_max) + "\n";
  out += "  tools:    " + join_tools(recipe.tools) + "\n";
  out += "  build:    " + std::to_string(recipe.build_steps.size()) +
         " step(s)\n";
  out += "  port:     " +
         (recipe.port == 0 ? std::string("auto")
                           : std::to_string(recipe.port)) +
         "\n";
  return out;
}

nlohmann::json manifest_json(const Recipe& recipe) {
  nlohmann::json doc = json_document("check-manifest");
  doc["recipe"] = recipe_to_json(recipe);
  return doc;
}

}
