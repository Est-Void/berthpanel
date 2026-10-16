#include "recipes.h"

#include <algorithm>
#include <string>

#include "../core/names.h"
#include "../core/netlog.h"
#include "../util/argv.h"
#include "../util/ascii.h"
#include "../util/strutil.h"
#include "net/fetch.h"
#include "units.h"
#include "validate.h"

namespace berth {

namespace {

bool language_supported(const std::string& language) {
  return language == "go" || language == "node" || language == "python" ||
         language == "prebuilt";
}

bool valid_sha256(const std::string& value) {
  return value.size() == 64 && all_chars(value, is_lower_hex);
}

// memory_max must look like systemd's: digits with an optional K/M/G/T.
bool valid_memory_max(const std::string& value) {
  if (value.empty() || value.size() > 16)
    return false;
  std::size_t index = 0;
  while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
    ++index;
  }
  if (index == 0)
    return false;
  const std::string suffix = value.substr(index);
  if (suffix.empty())
    return true;
  return suffix == "K" || suffix == "M" || suffix == "G" || suffix == "T";
}

bool pin_is_allowlisted(const std::string& url) {
  const Result<HttpUrl> parsed = parse_http_url(url);
  if (parsed.is_err())
    return false;
  return classify_endpoint(parsed.value().host) == EndpointClass::kAllowed;
}

Result<std::vector<std::vector<std::string>>> steps_from_json(
    const nlohmann::json& doc, const char* field) {
  std::vector<std::vector<std::string>> steps;
  if (!doc.contains(field))
    return steps;
  const nlohmann::json& list = doc.at(field);
  if (!list.is_array()) {
    return Error(std::string("recipe: ") + field + " must be an array");
  }
  for (const nlohmann::json& item : list) {
    if (!item.is_array() || item.empty()) {
      return Error(std::string("recipe: ") + field +
                   " entries must be non-empty argv arrays");
    }
    std::vector<std::string> step;
    for (const nlohmann::json& element : item) {
      step.push_back(element.get<std::string>());
    }
    steps.push_back(std::move(step));
  }
  return steps;
}

}
Result<std::vector<std::string>> split_command_template(
    const std::string_view tmpl) {
  std::vector<std::string> tokens;
  std::string current;
  bool in_quotes = false;
  for (const char c : tmpl) {
    if (c == ' ' || c == '\t') {
      if (in_quotes) {
        current += c;
        continue;
      }
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    if (c == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    current += c;
  }
  if (in_quotes)
    return Error("recipe: unbalanced quote in command template");
  if (!current.empty())
    tokens.push_back(current);
  if (tokens.empty())
    return Error("recipe: empty command template");
  return tokens;
}

std::vector<std::string> tools_required_by(
    const std::vector<std::string>& step) {
  if (step.empty())
    return {};
  const std::string program = program_name(step);
  if (program == "sh" || program == "bash") {
    return {"sh"};
  }
  if (program == "python") {
    // A venv interpreter IS python3, or venv-based recipes fail their own tool.
    return {"python3"};
  }
  return {program};
}

bool tool_covers_step(const Recipe& recipe,
                      const std::vector<std::string>& step) {
  if (step.empty())
    return false;
  for (const std::string& needed : tools_required_by(step)) {
    if (std::find(recipe.tools.begin(), recipe.tools.end(), needed) ==
        recipe.tools.end()) {
      return false;
    }
  }
  return true;
}

namespace {

Result<void> validate_identity(const Recipe& recipe) {
  if (!validate_unit_name(recipe.service_name)) {
    return Error("recipe: invalid service_name: '" + recipe.service_name + "'");
  }
  const Result<void> repo = validate_owner_repo(recipe.owner_repo());
  if (repo.is_err())
    return std::move(repo).error();
  const Result<void> sha = validate_commit_sha(recipe.commit);
  if (sha.is_err())
    return std::move(sha).error();
  if (!language_supported(recipe.language)) {
    return Error("recipe: unsupported language: '" + recipe.language + "'");
  }
  if (!valid_memory_max(recipe.res.memory_max) || recipe.res.tasks_max == 0) {
    return Error("recipe: " + recipe.service_name +
                 " must declare memory_max and tasks_max (§8)");
  }
  return {};
}

// Returns the start template joined by spaces: the bind and {port} checks read
// it, and a split that fails is already a shell-syntax rejection.
Result<std::string> validate_start_argv(const Recipe& recipe) {
  const Result<std::vector<std::string>> start =
      split_command_template(recipe.start_cmd);
  if (start.is_err())
    return std::move(start).error();
  for (const std::string& token : start.value()) {
    if (token.find('\'') != std::string::npos ||
        token.find(';') != std::string::npos ||
        token.find('|') != std::string::npos ||
        token.find('$') != std::string::npos ||
        token.find('`') != std::string::npos ||
        token.find('>') != std::string::npos) {
      return Error(
          "recipe: start_cmd must be an argv template, not shell syntax: '" +
          token + "'");
    }
  }
  if (!matched_dangerous_patterns(recipe.start_cmd).empty()) {
    return Error("recipe: start_cmd matches the dangerous-command denylist");
  }
  std::string joined;
  for (const std::string& token : start.value()) {
    joined += token;
    joined += ' ';
  }
  return joined;
}

Result<void> validate_build_steps(const Recipe& recipe) {
  if (recipe.port != 0 && recipe.port < 1024) {
    return Error("recipe: privileged ports are unreachable for a user unit");
  }
  std::vector<std::vector<std::string>> steps = recipe.pre_build;
  steps.insert(steps.end(), recipe.build_steps.begin(),
               recipe.build_steps.end());
  for (const std::vector<std::string>& step : steps) {
    if (!matched_dangerous_patterns(step).empty()) {
      return Error(
          "recipe: a build step matches the dangerous-command denylist");
    }
    if (!tool_covers_step(recipe, step)) {
      return Error("recipe: build step program '" + program_name(step) +
                   "' is not declared in tools (§8: a green preflight with an "
                   "undeclared tool fails at build time)");
    }
  }
  for (const std::vector<std::string>& step : recipe.prefetch_steps) {
    if (!matched_dangerous_patterns(step).empty()) {
      return Error("recipe: a prefetch step matches the denylist");
    }
  }
  return {};
}

Result<void> validate_pins(const Recipe& recipe) {
  for (const ContentPin& pin : recipe.plugins) {
    if (!pin_is_allowlisted(pin.url) || !valid_sha256(pin.sha256)) {
      return Error(
          "recipe: plugin pin must be an allowlisted https url with a sha256");
    }
  }
  if (recipe.prebuilt.has_value()) {
    const PrebuiltBinary& prebuilt = recipe.prebuilt.value();
    if (!pin_is_allowlisted(prebuilt.url) || !valid_sha256(prebuilt.sha256)) {
      return Error(
          "recipe: prebuilt binary must be an allowlisted https url with a "
          "sha256 pin");
    }
    if (prebuilt.member.empty()) {
      return Error("recipe: prebuilt binary needs a member file name");
    }
    if (recipe.language != "prebuilt") {
      return Error("recipe: prebuilt binary implies language=prebuilt");
    }
  }
  if (recipe.language == "prebuilt" && !recipe.prebuilt.has_value()) {
    return Error("recipe: language=prebuilt needs a prebuilt pin");
  }
  if (recipe.binary_name.empty()) {
    return Error("recipe: " + recipe.service_name +
                 " must declare binary_name (the file the build produces)");
  }
  if (recipe.language != "prebuilt" && recipe.build_steps.empty()) {
    return Error("recipe: a built-from-source recipe needs build_steps");
  }
  return {};
}

// §8: loopback only. No silent default: with no bind flag, DECLARE it.
Result<void> validate_bind(const Recipe& recipe, const std::string& joined) {
  if (recipe.bind == "0.0.0.0") {
    return Error("recipe: " + recipe.service_name +
                 " must not bind a wildcard address");
  }
  if (!recipe.bind.empty() && recipe.bind != "127.0.0.1") {
    return Error("recipe: bind must be '127.0.0.1' (loopback) or empty, got '" +
                 recipe.bind + "'");
  }
  const bool loopback = joined.find("127.0.0.1") != std::string::npos ||
                        joined.find("localhost") != std::string::npos ||
                        recipe.bind == "127.0.0.1";
  const bool wildcard = joined.find("0.0.0.0") != std::string::npos ||
                        joined.find("--addr :") != std::string::npos;
  if (!loopback) {
    return Error("recipe: " + recipe.service_name +
                 " must bind loopback explicitly (127.0.0.1) — a wildcard "
                 "bind would expose the service on every interface");
  }
  if (wildcard) {
    return Error("recipe: " + recipe.service_name +
                 " must not bind a wildcard address");
  }
  // {port} is required only when the deploy picks the port.
  if (recipe.port == 0 && joined.find("{port}") == std::string::npos) {
    return Error("recipe: start_cmd must carry the {port} placeholder");
  }
  return {};
}

}

Result<void> validate_recipe(const Recipe& recipe) {
  const Result<void> identity = validate_identity(recipe);
  if (identity.is_err())
    return std::move(identity).error();
  const Result<std::string> start = validate_start_argv(recipe);
  if (start.is_err())
    return std::move(start).error();
  const Result<void> steps = validate_build_steps(recipe);
  if (steps.is_err())
    return std::move(steps).error();
  const Result<void> pins = validate_pins(recipe);
  if (pins.is_err())
    return std::move(pins).error();
  return validate_bind(recipe, start.value());
}

namespace {

Result<void> steps_into(std::vector<std::vector<std::string>>& target,
                       const nlohmann::json& doc, const char* field) {
  const Result<std::vector<std::vector<std::string>>> parsed =
      steps_from_json(doc, field);
  if (parsed.is_err())
    return std::move(parsed).error();
  target = std::move(parsed).value();
  return {};
}

Result<PrebuiltBinary> prebuilt_from_json(const nlohmann::json& prebuilt) {
  ToolArchive archive = ToolArchive::kTarGz;
  if (prebuilt.contains("archive")) {
    const std::string value = prebuilt.at("archive").get<std::string>();
    if (value == "tar.gz") {
      archive = ToolArchive::kTarGz;
    } else if (value == "raw") {
      archive = ToolArchive::kRaw;
    } else {
      return Error(
          "recipe: prebuilt.archive must be 'tar.gz' or 'raw', got '" + value +
          "'");
    }
  }
  return PrebuiltBinary{prebuilt.at("url").get<std::string>(),
                        prebuilt.at("sha256").get<std::string>(),
                        prebuilt.at("member").get<std::string>(), archive};
}

}

Result<Recipe> recipe_from_json(const nlohmann::json& doc) {
  if (!doc.is_object())
    return Error("recipe: not an object");
  Recipe r;
  r.owner = doc.value("owner", "");
  r.name = doc.value("name", "");
  r.commit = doc.value("commit", "");
  r.language = doc.value("language", "");
  r.service_name = doc.value("service_name", "");
  r.display_name = doc.value("display_name", "");
  r.description = doc.value("description", "");
  r.binary_name = doc.value("binary_name", "");
  r.start_cmd = doc.value("start_cmd", "");
  r.bind = doc.value("bind", "");
  r.health_path = doc.value("health_path", "/");
  r.port = doc.value("port", static_cast<std::uint16_t>(0));
  for (const nlohmann::json& tool :
       doc.value("tools", nlohmann::json::array())) {
    r.tools.push_back(tool.get<std::string>());
  }
  Result<void> steps = steps_into(r.pre_build, doc, "pre_build");
  if (steps.is_err())
    return std::move(steps).error();
  steps = steps_into(r.build_steps, doc, "build_steps");
  if (steps.is_err())
    return std::move(steps).error();
  steps = steps_into(r.prefetch_steps, doc, "prefetch_steps");
  if (steps.is_err())
    return std::move(steps).error();
  if (doc.contains("plugins")) {
    for (const nlohmann::json& pin : doc.at("plugins")) {
      r.plugins.push_back({pin.at("url").get<std::string>(),
                           pin.at("sha256").get<std::string>()});
    }
  }
  if (doc.contains("prebuilt")) {
    const Result<PrebuiltBinary> prebuilt =
        prebuilt_from_json(doc.at("prebuilt"));
    if (prebuilt.is_err())
      return std::move(prebuilt).error();
    r.prebuilt = std::move(prebuilt).value();
  }
  if (doc.contains("res")) {
    r.res.memory_max = doc.at("res").value("memory_max", "");
    r.res.tasks_max = doc.at("res").value("tasks_max", 0U);
  }
  return r;
}

nlohmann::json recipe_to_json(const Recipe& recipe) {
  nlohmann::json doc = {
      {"owner", recipe.owner},
      {"name", recipe.name},
      {"commit", recipe.commit},
      {"language", recipe.language},
      {"service_name", recipe.service_name},
      {"display_name", recipe.display_name},
      {"description", recipe.description},
      {"binary_name", recipe.binary_name},
      {"start_cmd", recipe.start_cmd},
      {"health_path", recipe.health_path},
      {"port", recipe.port},
      {"tools", recipe.tools},
      {"res",
       {{"memory_max", recipe.res.memory_max},
        {"tasks_max", recipe.res.tasks_max}}},
  };
  const auto to_json = [](const std::vector<std::vector<std::string>>& steps) {
    nlohmann::json array = nlohmann::json::array();
    for (const std::vector<std::string>& step : steps) {
      array.push_back(step);
    }
    return array;
  };
  doc["pre_build"] = to_json(recipe.pre_build);
  doc["build_steps"] = to_json(recipe.build_steps);
  doc["prefetch_steps"] = to_json(recipe.prefetch_steps);
  // Only a DECLARED bind is written, so recipe JSON does not churn.
  if (!recipe.bind.empty()) {
    doc["bind"] = recipe.bind;
  }
  if (recipe.prebuilt.has_value()) {
    doc["prebuilt"] = {{"url", recipe.prebuilt->url},
                       {"sha256", recipe.prebuilt->sha256},
                       {"member", recipe.prebuilt->member}};
    // kTarGz is the historical default: older recipe JSON stays byte-identical.
    if (recipe.prebuilt->archive == ToolArchive::kRaw) {
      doc["prebuilt"]["archive"] = "raw";
    }
  }
  if (!recipe.plugins.empty()) {
    nlohmann::json pins = nlohmann::json::array();
    for (const ContentPin& pin : recipe.plugins) {
      pins.push_back({{"url", pin.url}, {"sha256", pin.sha256}});
    }
    doc["plugins"] = pins;
  }
  return doc;
}

}
