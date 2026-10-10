#pragma once

// src/berth/recipes.h — the recipe schema (dev.md §8) and THE validator.
//
// The catalog contract tests of §8 became this function: resource limits,
// tools-covering-build-steps, loopback-only binding, pinned commit, pinned
// content hashes and the dangerous-command denylist are all enforced HERE,
// at load time, so a bad recipe cannot reach a deploy. One validator,
// used by the builtin catalog and (stage 4B) by user recipes alike — two
// validators that disagree is the §11 trap.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include "../util/result.h"
#include "toolbox.h"

namespace berth {

struct ResourceLimits {
  std::string memory_max;
  std::uint32_t tasks_max = 0;
};

struct ContentPin {
  std::string url;
  std::string sha256;
};

// A release asset fetched through toolbox (prebuilt recipes: no compiler
// on the host, so the toolchain requirement disappears).
struct PrebuiltBinary {
  std::string url;
  std::string sha256;
  std::string member;  // member of the tar.gz, or the destination file for kRaw
  ToolArchive archive = ToolArchive::kTarGz;  // absent JSON field = kTarGz
};

struct Recipe {
  std::string owner;
  std::string name;
  std::string commit;  // pinned 40-hex SHA, never a branch (§6.6)
  std::string language;
  std::string service_name;
  std::string display_name;
  std::string description;
  std::vector<std::vector<std::string>> pre_build;
  std::vector<std::vector<std::string>> build_steps;
  std::vector<std::vector<std::string>> prefetch_steps;
  std::vector<ContentPin> plugins;
  std::optional<PrebuiltBinary> prebuilt;
  std::string binary_name;  // file the build produces inside the work dir
  std::string
      start_cmd;  // placeholders {bin} {venv} {python} {project} {port} {self}
  // The author's explicit declaration of the address the app binds ITSELF
  // when start_cmd carries no bind token. "" = not declared; only
  // "127.0.0.1" is a valid declaration (the validator refuses anything
  // else), so loopback is always stated, never a silent default (§8).
  std::string bind;
  std::string health_path = "/";
  std::uint16_t port = 0;  // 0 = pick a free one at deploy time
  std::vector<std::string> tools;
  ResourceLimits res;

  [[nodiscard]] std::string owner_repo() const { return owner + "/" + name; }
};

// Splits a command TEMPLATE into argv tokens. Only the template is split,
// before substitution, because a substituted path may legitimately contain
// a space: splitting afterwards would destroy that information (§11).
[[nodiscard]] Result<std::vector<std::string>> split_command_template(
    std::string_view tmpl);

[[nodiscard]] Result<void> validate_recipe(const Recipe& recipe);

// The interface every build step's program must satisfy: present in
// `tools`. Exposed so preflight and the catalog tests agree.
[[nodiscard]] bool tool_covers_step(const Recipe& recipe,
                                    const std::vector<std::string>& step);

// The tool names a step requires from `tools` (the venv interpreter IS
// python3; a sh wrapper IS sh). Exposed so detect can heal its own drafts
// instead of ever handing out an invalid one (stage 4.20).
[[nodiscard]] std::vector<std::string> tools_required_by(
    const std::vector<std::string>& step);

[[nodiscard]] Result<Recipe> recipe_from_json(const nlohmann::json& doc);
[[nodiscard]] nlohmann::json recipe_to_json(const Recipe& recipe);

}  // namespace berth