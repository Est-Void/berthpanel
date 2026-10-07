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

// Lockfiles decide the package manager (plan 4.20); an untracked
// package.json defaults to bun — npm is not in toolcheck, so it can never
// be more than an unverified guess.
enum class NodeTool { kBun, kPnpm };

NodeTool node_tool_of(const std::filesystem::path& dir) {
  if (present(dir / "pnpm-lock.yaml"))
    return NodeTool::kPnpm;
  return NodeTool::kBun;
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
std::uint16_t dockerfile_port_hint(const std::filesystem::path& dir) {
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
        return static_cast<std::uint16_t>(parsed);
      }
      break;  // first numeric token decided; ignore the rest of the line
    }
  }
  return 0;
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

Result<Recipe> detect_recipe(const DetectRequest& request) {
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

  Recipe r;
  r.owner = request.owner;
  r.name = request.name;
  // The unit stem: the mandatory prefix (stage 4.19), derived from the
  // repo name. validate_owner_repo already bounded the characters.
  r.service_name = force_unit_prefix(request.name);
  r.commit = request.commit;
  r.display_name = request.name;
  r.health_path = "/";
  r.port = dockerfile_port_hint(dir);
  r.res = ResourceLimits{.memory_max = "512M", .tasks_max = 300};

  if (present(dir / "go.mod")) {
    Result<std::string> module = go_module_name(dir);
    if (module.is_err())
      return std::move(module).error();
    const std::string binary =
        binary_name_from_module(module.value(), request.name);
    r.language = "go";
    r.tools = {"go"};
    r.build_steps = {{"go", "build", "-o", "{project}/" + binary, "."}};
    r.binary_name = binary;
    r.start_cmd = "{self} --addr 127.0.0.1 --port {port}";
    r.description =
        "Detected Go app from " + request.owner + "/" + request.name;
  } else if (present(dir / "package.json")) {
    Result<PackageJson> pkg = read_package_json(dir);
    if (pkg.is_err())
      return std::move(pkg).error();
    const NodeTool tool = node_tool_of(dir);
    const std::string runner = tool == NodeTool::kPnpm ? "pnpm" : "bun";
    r.language = "node";
    r.tools = {runner};
    std::vector<std::vector<std::string>> build = {{runner, "install"}};
    if (pkg.value().has_build_script) {
      build.push_back({runner, "run", "build"});
    }
    r.build_steps = build;
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
  } else if (present(dir / "pyproject.toml") ||
             present(dir / "requirements.txt")) {
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
    r.description =
        "Detected Python app from " + request.owner + "/" + request.name;
  } else {
    return Error("detect: no supported stack in " + dir.string() +
                 " (looked for go.mod, package.json, pyproject.toml, "
                 "requirements.txt)");
  }

  // The draft must be deployable as-is (quick-accept, stage 4.21) — if
  // our own heuristics produce an invalid recipe, that is a bug here and
  // not something to hand to the user.
  Result<void> valid = validate_recipe(r);
  if (valid.is_err()) {
    return Error("detect: internal error, the draft failed validation: " +
                 valid.error().ctx());
  }
  return r;
}

}  // namespace berth
