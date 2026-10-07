// src/berth/detect.cpp — see detect.h for the contract.

#include "detect.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "../core/names.h"
#include "../util/ascii.h"
#include "../util/fs.h"
#include "../util/strutil.h"
#include "units.h"
#include "validate.h"

namespace berth {

namespace {

bool present(const std::filesystem::path& path) {
  std::error_code ec;
  const bool there = std::filesystem::exists(path, ec);
  return !ec && there;
}

Result<std::string> read_small(const std::filesystem::path& path) {
  if (!present(path))
    return std::string();
  Result<std::string> body = read_file(path);
  if (body.is_err())
    return std::move(body).error();
  // A stack marker file larger than this is not a config, it is an
  // accident (or a trap). Refuse rather than burn memory on it.
  if (body.value().size() > 1024 * 1024) {
    return Error("detect: " + path.string() + " is larger than 1 MiB");
  }
  return body;
}

// The build output file name: the last segment of the go module path,
// reduced to characters a unit and an ExecStart can live with.
std::string binary_name_from_module(const std::string& module_path,
                                    const std::string& fallback) {
  std::string base = module_path;
  const std::size_t slash = base.rfind('/');
  if (slash != std::string::npos)
    base = base.substr(slash + 1);
  if (base.empty())
    return fallback;
  std::string clean;
  for (const char c : base) {
    if (is_unit_stem_char(c))
      clean += c;
  }
  return clean.empty() ? fallback : clean;
}

Result<std::string> go_module_name(const std::filesystem::path& dir) {
  Result<std::string> body = read_small(dir / "go.mod");
  if (body.is_err())
    return std::move(body).error();
  for (const std::string& line : split(body.value(), "\n")) {
    if (line.rfind("module ", 0) == 0) {
      return std::string(trim(std::string_view(line).substr(7)));
    }
  }
  return std::string();
}

// Lockfiles decide the package manager (plan 4.20). A package-lock.json
// is NOT guessed into bun: npm is not in the v1 toolchain, and a wrong
// guess builds a tree the repo never shipped — that is a loud Err.
enum class NodeTool { kBun, kPnpm };

Result<NodeTool> node_tool_of(const std::filesystem::path& dir) {
  if (present(dir / "pnpm-lock.yaml"))
    return NodeTool::kPnpm;
  if (present(dir / "bun.lockb"))
    return NodeTool::kBun;
  if (present(dir / "package-lock.json")) {
    return Error(
        "detect: package-lock.json means npm, but npm is not in the v1 "
        "toolchain (bun/pnpm only); publish a pnpm or bun lockfile, or "
        "hand-write the recipe");
  }
  return NodeTool::kBun;  // untracked package.json defaults to bun
}

struct PackageJson {
  std::string main = "index.js";
  bool has_build_script = false;
  bool has_start_script = false;
};

Result<PackageJson> read_package_json(const std::filesystem::path& dir) {
  PackageJson pkg;
  Result<std::string> body = read_small(dir / "package.json");
  if (body.is_err())
    return std::move(body).error();
  if (body.value().empty())
    return pkg;  // absent
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(body.value());
  } catch (const nlohmann::json::exception& e) {
    return Error("detect: package.json: " + std::string(e.what()));
  }
  if (!doc.is_object())
    return Error("detect: package.json is not an object");
  if (doc.contains("main") && doc["main"].is_string() &&
      !doc["main"].get<std::string>().empty()) {
    pkg.main = doc["main"].get<std::string>();
    // No "../": the entry must stay inside the work dir (§11: every value
    // that reaches a path is validated here, in the producer).
    if (pkg.main.find("..") != std::string::npos || pkg.main.front() == '/') {
      return Error(
          "detect: package.json main must be a relative path "
          "inside the project: " +
          pkg.main);
    }
  }
  if (doc.contains("scripts") && doc["scripts"].is_object()) {
    pkg.has_build_script = doc["scripts"].contains("build");
    pkg.has_start_script = doc["scripts"].contains("start");
  }
  return pkg;
}

// Dockerfile EXPOSE is a hint, nothing more (plan 4.20): the first
// well-formed port ≥ 1024 wins; everything else is ignored.
std::uint16_t dockerfile_port_hint(const std::filesystem::path& dir,
                                   std::vector<std::string>* warnings) {
  Result<std::string> body = read_small(dir / "Dockerfile");
  if (body.is_err() || body.value().empty())
    return 0;
  for (const std::string& raw : split(body.value(), "\n")) {
    std::string line(trim(raw));
    if (line.rfind("EXPOSE", 0) != 0)
      continue;
    for (const std::string& token : split(line, " ")) {
      if (token.empty() || token == "EXPOSE")
        continue;
      if (token.find_first_not_of("0123456789") != std::string::npos)
        continue;
      const long parsed = std::strtol(token.c_str(), nullptr, 10);
      if (parsed >= 1024 && parsed <= 65535) {
        warnings->push_back("Dockerfile EXPOSE " + std::to_string(parsed) +
                            " — the draft deploys on this port unless "
                            "another one is forced");
        return static_cast<std::uint16_t>(parsed);
      }
      break;  // first numeric token decided; ignore the rest of the line
    }
  }
  return 0;
}

// Shell metacharacters: a CMD token carrying any of these is not a safe
// argv word, so the hint is refused rather than re-quoted by hand.
bool has_shell_metachar(const std::string& token) {
  return token.find_first_of(";|&><$`\"'\\\t\n\r") != std::string::npos ||
         token.find(' ') != std::string::npos;
}

// Dockerfile CMD → flag hint for start_cmd, exec form first (a JSON argv
// array), then the bare shell form when it parses into argv words without
// shell syntax. Anything else is refused with a reason — never guessed.
Result<std::vector<std::string>> dockerfile_cmd_hint(
    const std::filesystem::path& dir) {
  Result<std::string> body = read_small(dir / "Dockerfile");
  if (body.is_err() || body.value().empty())
    return std::vector<std::string>();
  for (const std::string& raw : split(body.value(), "\n")) {
    std::string line(trim(raw));
    if (line.rfind("CMD", 0) != 0)
      continue;
    std::string rest(trim(std::string_view(line).substr(3)));
    if (rest.empty())
      continue;
    if (rest.front() == '[') {
      nlohmann::json doc;
      try {
        doc = nlohmann::json::parse(rest);
      } catch (const nlohmann::json::exception&) {
        return Error("detect: Dockerfile CMD is a malformed JSON array");
      }
      if (!doc.is_array() || doc.empty() ||
          !std::all_of(doc.begin(), doc.end(),
                       [](const nlohmann::json& item) {
                         return item.is_string();
                       })) {
        return Error(
            "detect: Dockerfile CMD exec form must be a non-empty array of "
            "strings");
      }
      std::vector<std::string> argv;
      for (const auto& item : doc) {
        argv.push_back(item.get<std::string>());
      }
      return argv;
    }
    // Shell form: only accept plain whitespace-separated words.
    if (rest.find_first_of(";|&><$`\"'\\") != std::string::npos) {
      return Error("detect: Dockerfile CMD uses shell syntax; write the "
                   "start command into the recipe by hand");
    }
    std::vector<std::string> argv;
    for (const std::string& token : split(rest, " ")) {
      if (!token.empty())
        argv.push_back(token);
    }
    if (argv.empty())
      continue;
    return argv;
  }
  return std::vector<std::string>();
}

// A draft start_cmd may take its flags from CMD only when it still binds
// loopback: a hint must never draft a wildcard bind (the validator would
// refuse it anyway — refusing it here says WHY).
bool binds_loopback(const std::string& start_cmd) {
  if (start_cmd.find("0.0.0.0") != std::string::npos ||
      start_cmd.find("::") != std::string::npos ||
      start_cmd.find(" :") != std::string::npos) {
    return false;
  }
  return start_cmd.find("127.0.0.1") != std::string::npos ||
         start_cmd.find("localhost") != std::string::npos;
}

std::string python_entry_script(const std::filesystem::path& dir) {
  if (present(dir / "main.py"))
    return "main.py";
  if (present(dir / "app.py"))
    return "app.py";
  if (present(dir / "wsgi.py"))
    return "wsgi.py";
  return "main.py";  // the default the wizard tells the user to check
}

}  // namespace

Result<RecipeDraft> detect_recipe(const DetectRequest& request) {
  const Result<void> coords =
      validate_owner_repo(request.owner + "/" + request.name);
  if (coords.is_err())
    return std::move(coords).error();
  const Result<void> sha = validate_commit_sha(request.commit);
  if (sha.is_err())
    return std::move(sha).error();

  std::error_code ec;
  if (!std::filesystem::is_directory(request.dir, ec) || ec) {
    return Error("detect: not a directory: " + request.dir.string());
  }
  const std::filesystem::path dir = request.dir;

  // One stack family per draft. Two families at once means the heuristic
  // would silently pick one — that decision belongs to the user.
  const bool go_stack = present(dir / "go.mod");
  const bool node_stack = present(dir / "package.json");
  const bool python_stack =
      present(dir / "pyproject.toml") || present(dir / "requirements.txt");
  int families = (go_stack ? 1 : 0) + (node_stack ? 1 : 0) +
                 (python_stack ? 1 : 0);
  if (families > 1) {
    std::vector<std::string> markers;
    if (go_stack)
      markers.push_back("go.mod");
    if (node_stack)
      markers.push_back("package.json");
    if (python_stack)
      markers.push_back("pyproject.toml/requirements.txt");
    return Error("detect: ambiguous stack: " + join(markers, " + ") +
                 " in " + dir.string() +
                 "; remove or rename the marker you do not mean");
  }

  RecipeDraft draft;
  Recipe& r = draft.recipe;
  std::vector<std::string>& warnings = draft.warnings;
  r.owner = request.owner;
  r.name = request.name;
  // The unit stem: the mandatory prefix (stage 4.19), derived from the
  // repo name. validate_owner_repo already bounded the characters.
  r.service_name = force_unit_prefix(request.name);
  r.commit = request.commit;
  r.display_name = request.name;
  r.health_path = "/";
  r.port = dockerfile_port_hint(dir, &warnings);
  r.res = ResourceLimits{.memory_max = "512M", .tasks_max = 300};

  if (go_stack) {
    Result<std::string> module = go_module_name(dir);
    if (module.is_err())
      return std::move(module).error();
    const std::string binary =
        binary_name_from_module(module.value(), request.name);
    r.language = "go";
    r.tools = {"go"};
    r.build_steps = {{"go", "build", "-o", "{project}/" + binary, "."}};
    r.prefetch_steps = {{"go", "mod", "download"}};
    r.binary_name = binary;
    // CMD may know the real flags; the loopback template is the fallback.
    const Result<std::vector<std::string>> cmd = dockerfile_cmd_hint(dir);
    if (cmd.is_err())
      warnings.push_back(cmd.error().ctx());
    std::string start = "{self} --addr 127.0.0.1 --port {port}";
    if (cmd.is_ok() && cmd.value().size() >= 2) {
      std::string hinted = "{self}";
      bool usable = true;
      for (std::size_t i = 1; i < cmd.value().size(); ++i) {
        if (has_shell_metachar(cmd.value()[i])) {
          usable = false;
          break;
        }
        hinted += " " + cmd.value()[i];
      }
      if (usable && binds_loopback(hinted)) {
        // The template contract requires {port}: the deploy substitutes
        // the picked port there, hinted or not.
        if (hinted.find("{port}") == std::string::npos) {
          hinted += " --port {port}";
        }
        start = hinted;
        warnings.push_back(
            "start_cmd flags taken from Dockerfile CMD — verify they match "
            "the app");
      } else if (usable) {
        warnings.push_back(
            "Dockerfile CMD does not bind loopback; using the default "
            "template — the app may need its bind flag edited");
      }
    }
    r.start_cmd = start;
    r.description =
        "Detected Go app from " + request.owner + "/" + request.name;
  } else if (node_stack) {    Result<NodeTool> tool = node_tool_of(dir);
    if (tool.is_err())
      return std::move(tool).error();
    Result<PackageJson> pkg = read_package_json(dir);
    if (pkg.is_err())
      return std::move(pkg).error();
    const std::string runner =
        tool.value() == NodeTool::kPnpm ? "pnpm" : "bun";
    r.language = "node";
    r.tools = {runner};
    std::vector<std::vector<std::string>> build = {{runner, "install"}};
    if (pkg.value().has_build_script) {
      build.push_back({runner, "run", "build"});
    }
    r.build_steps = build;
    // §6.5 on the host: dependencies download, never execute. The same
    // steps stay out of the sandbox build.
    r.prefetch_steps = {{runner, "install", "--ignore-scripts"}};
    // The entry is data inside the work dir: {self} must point at a real
    // file, so for node the "binary" is the entry script itself.
    r.binary_name = pkg.value().main;
    if (pkg.value().has_start_script) {
      r.start_cmd = runner + " run start --addr 127.0.0.1 --port {port}";
    } else {
      r.start_cmd = runner + " {project}/" + pkg.value().main +
                    " --addr 127.0.0.1 --port {port}";
    }
    r.description =
        "Detected Node app from " + request.owner + "/" + request.name;
  } else if (python_stack) {
    r.language = "python";
    r.tools = {"python3"};
    r.binary_name = "venv/bin/python";
    std::vector<std::vector<std::string>> build = {
        {"python3", "-m", "venv", "{project}/venv"}};
    if (present(dir / "requirements.txt")) {
      build.push_back({"{project}/venv/bin/python", "-m", "pip", "install",
                       "-r", "{project}/requirements.txt"});
    } else {
      build.push_back(
          {"{project}/venv/bin/python", "-m", "pip", "install", "{project}"});
    }
    r.build_steps = build;
    r.start_cmd = "{self} {project}/" + python_entry_script(dir) +
                  " --addr 127.0.0.1 --port {port}";
    warnings.push_back(
        "python dependencies install inside the sandbox; offline builds "
        "need a prefetch step, which this draft does not add yet");
    r.description =
        "Detected Python app from " + request.owner + "/" + request.name;
  } else {
    return Error("detect: no supported stack in " + dir.string() +
                 " (looked for go.mod, package.json, pyproject.toml, "
                 "requirements.txt)");
  }
  warnings.push_back(
      "start_cmd is an assumption — verify the app's real flags before "
      "deploying");

  // The draft must be deployable as-is (quick-accept, stage 4.21): heal
  // the tools coverage first, so our own heuristics can never hand out an
  // invalid recipe. A failure after healing is a bug here, not user input.
  for (const std::vector<std::string>& step : r.build_steps) {
    for (const std::string& needed : tools_required_by(step)) {
      if (std::find(r.tools.begin(), r.tools.end(), needed) == r.tools.end()) {
        r.tools.push_back(needed);
      }
    }
  }
  for (const std::vector<std::string>& step : r.prefetch_steps) {
    for (const std::string& needed : tools_required_by(step)) {
      if (std::find(r.tools.begin(), r.tools.end(), needed) == r.tools.end()) {
        r.tools.push_back(needed);
      }
    }
  }
  Result<void> valid = validate_recipe(r);
  if (valid.is_err()) {
    return Error("detect: internal error, the draft failed validation: " +
                 valid.error().ctx());
  }
  return draft;
}

}  // namespace berth
