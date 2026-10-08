// src/tui/view_catalog.cpp — see view_catalog.h for the contract.

#include "view_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../berth/catalog.h"
#include "../berth/userrecipes.h"
#include "../util/strutil.h"

namespace tui {

namespace {

constexpr std::size_t kCommitShortLen = 7;

// Column widths of the list table. The service name is the widest fixed
// column; everything after it is short and fixed-width so the table does not
// jitter as the cursor moves.
constexpr std::size_t kNameWidth = 24;
constexpr std::size_t kLangWidth = 10;
constexpr std::size_t kLimitsWidth = 16;

constexpr std::string_view kUnknown = "-";

void pad_to(std::string& out, const std::string_view text,
            const std::size_t width) {
  out.append(text);
  if (text.size() < width) {
    out.append(width - text.size(), ' ');
  }
}

std::string short_commit(const std::string& commit) {
  if (commit.size() <= kCommitShortLen) {
    return commit;
  }
  return commit.substr(0, kCommitShortLen);
}

std::string limits_cell(const berth::Recipe& recipe) {
  if (recipe.res.memory_max.empty() && recipe.res.tasks_max == 0) {
    return std::string(kUnknown);
  }
  return recipe.res.memory_max + "/" + std::to_string(recipe.res.tasks_max);
}

[[noreturn]] void abort_out_of_range() {
  std::fputs("tui: invariant violated: catalog index out of range\n", stderr);
  std::abort();
}

const berth::Recipe& recipe_at(const Catalog& catalog,
                               const std::size_t index) {
  if (index >= catalog.recipes.size()) {
    abort_out_of_range();
  }
  return catalog.recipes[index];
}

void append_steps(std::vector<std::string>& out, const std::string_view heading,
                  const std::vector<std::vector<std::string>>& steps) {
  if (steps.empty()) {
    return;
  }
  out.emplace_back(std::string(heading));
  for (const std::vector<std::string>& step : steps) {
    out.emplace_back("    " + render_step(step));
  }
}

}  // namespace

berth::Result<Catalog> load_catalog(const berth::Paths& paths) {
  berth::Result<std::vector<berth::Recipe>> builtin = berth::builtin_catalog();
  if (builtin.is_err()) {
    return berth::with_context(std::move(builtin).error(),
                               "reading the builtin catalog");
  }
  berth::Result<std::vector<berth::Recipe>> user =
      berth::user_recipes_load(paths);
  if (user.is_err()) {
    return berth::with_context(std::move(user).error(), "reading user recipes");
  }

  Catalog catalog;
  catalog.recipes = std::move(builtin).value();
  catalog.sources.assign(catalog.recipes.size(), RecipeSource::kBuiltin);
  for (berth::Recipe& recipe : std::move(user).value()) {
    catalog.recipes.push_back(std::move(recipe));
    catalog.sources.push_back(RecipeSource::kUser);
  }
  return catalog;
}

std::string_view recipe_source_label(const RecipeSource source) {
  switch (source) {
    case RecipeSource::kBuiltin:
      return "builtin";
    case RecipeSource::kUser:
      return "user";
  }
  std::fputs("tui: invariant violated: unknown RecipeSource\n", stderr);
  std::abort();
}

std::string render_step(const std::vector<std::string>& step) {
  return berth::join(step, " ");
}

std::string catalog_header_line() {
  std::string head;
  pad_to(head, "SERVICE", kNameWidth);
  pad_to(head, "LANG", kLangWidth);
  pad_to(head, "PIN", kLimitsWidth);
  head.append("LIMITS");
  return head;
}

std::vector<std::string> catalog_row_lines(const Catalog& catalog) {
  std::vector<std::string> lines;
  lines.reserve(catalog.recipes.size());

  for (std::size_t i = 0; i < catalog.recipes.size(); ++i) {
    const berth::Recipe& recipe = catalog.recipes[i];
    std::string line;
    pad_to(
        line,
        recipe.display_name.empty() ? recipe.service_name : recipe.display_name,
        kNameWidth);
    pad_to(line, recipe.language.empty() ? kUnknown : recipe.language,
           kLangWidth);
    pad_to(line, short_commit(recipe.commit), kLimitsWidth);
    line.append(limits_cell(recipe));
    lines.push_back(std::move(line));
  }
  return lines;
}

std::vector<std::string> catalog_detail_lines(const Catalog& catalog,
                                              const std::size_t index) {
  const berth::Recipe& recipe = recipe_at(catalog, index);
  std::vector<std::string> out;

  out.emplace_back(recipe.display_name.empty() ? recipe.service_name
                                               : recipe.display_name);
  out.emplace_back("  source: " +
                   std::string(recipe_source_label(catalog.sources[index])));
  out.emplace_back("  unit:   " + recipe.service_name);
  // The actionable line goes first: the details pane clips at the box edge
  // on a short terminal, and this is the one line the user came for.
  out.emplace_back(
      "  deploy: switch to the Deploy tab and press Enter "
      "  (CLI: berthpanel __deploy " +
      recipe.service_name + ")");
  if (!recipe.description.empty()) {
    out.emplace_back("  " + recipe.description);
  }
  out.emplace_back("  repo:   " + recipe.owner_repo() + " @ " + recipe.commit);
  if (recipe.port != 0) {
    out.emplace_back("  port:   " + std::to_string(recipe.port));
  } else {
    out.emplace_back("  port:   free at deploy time");
  }
  out.emplace_back("  limits: " + recipe.res.memory_max + " / " +
                   std::to_string(recipe.res.tasks_max) +
                   " tasks_max (mandatory, §8)");

  out.emplace_back("");
  out.emplace_back("  tools: " + (recipe.tools.empty()
                                      ? std::string(kUnknown)
                                      : berth::join(recipe.tools, ", ")));
  append_steps(out, "  pre_build:", recipe.pre_build);
  append_steps(out, "  prefetch (host phase, never executed here):",
               recipe.prefetch_steps);
  append_steps(out, "  build:", recipe.build_steps);
  for (const berth::ContentPin& pin : recipe.plugins) {
    out.emplace_back("    plugin: " + pin.url + " sha256=" + pin.sha256);
  }
  if (recipe.prebuilt.has_value()) {
    out.emplace_back("    prebuilt: " + recipe.prebuilt->url +
                     " sha256=" + recipe.prebuilt->sha256 + " (" +
                     recipe.prebuilt->member + ")");
  }

  out.emplace_back("");
  out.emplace_back("  binary: " + recipe.binary_name);
  out.emplace_back("  start:  " + recipe.start_cmd);
  out.emplace_back("  health: " + recipe.health_path);
  return out;
}

}  // namespace tui