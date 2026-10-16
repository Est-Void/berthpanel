#pragma once

// THE validator (§8): limits, tools-covering build steps, loopback-only
// binding, pinned commit and content hashes, the dangerous-command denylist —
// enforced at load, by ONE validator for builtin and user recipes.

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

// A release asset fetched through toolbox: no compiler on the host.
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
  std::string binary_name;
  std::string
      start_cmd;  // placeholders {bin} {venv} {python} {project} {port} {self}
  // The author's declaration of the address the app binds ITSELF when start_cmd
  // has no bind token. Only "127.0.0.1" is valid, never a silent default (§8).
  std::string bind;
  std::string health_path = "/";
  std::uint16_t port = 0;  // 0 = pick a free one at deploy time
  std::vector<std::string> tools;
  ResourceLimits res;

  [[nodiscard]] std::string owner_repo() const { return owner + "/" + name; }
};

// Only the TEMPLATE is split: a substituted path may contain a space (§11).
[[nodiscard]] Result<std::vector<std::string>> split_command_template(
    std::string_view tmpl);

[[nodiscard]] Result<void> validate_recipe(const Recipe& recipe);

// Every build step's program must be present in `tools`.
[[nodiscard]] bool tool_covers_step(const Recipe& recipe,
                                    const std::vector<std::string>& step);

// The venv interpreter IS python3 and a sh wrapper IS sh.
[[nodiscard]] std::vector<std::string> tools_required_by(
    const std::vector<std::string>& step);

[[nodiscard]] Result<Recipe> recipe_from_json(const nlohmann::json& doc);
[[nodiscard]] nlohmann::json recipe_to_json(const Recipe& recipe);

}
