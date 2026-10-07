// Tests for detect.h (stage 4.20): stack heuristics → recipe DRAFT.
// Detection only reads fixture trees, never executes anything. Every
// draft must pass validate_recipe as-is — quick-accept (4.21) must be
// honest — so each test asserts the validator agrees with the heuristic.

#include "berth/detect.h"

#include <filesystem>
#include <string>

#include "berth/recipes.h"
#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::DetectRequest;
using berth::Recipe;
using berth::Result;

namespace {

const char* kGoMod = "module example.com/someone/myapp\n\ngo 1.22\n";

DetectRequest request_for(const std::filesystem::path& dir) {
  return DetectRequest{.dir = dir,
                       .owner = "someone",
                       .name = "myapp",
                       .commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01"};
}

}  // namespace

TEST(detect_go_module_produces_a_valid_draft) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());

  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().language == "go");
  CHECK(draft.value().tools == std::vector<std::string>{"go"});
  CHECK(draft.value().binary_name == "myapp");  // module path basename
  CHECK(draft.value().build_steps.size() == 1);
  CHECK(
      (draft.value().build_steps.front() ==
       std::vector<std::string>{"go", "build", "-o", "{project}/myapp", "."}));
  CHECK(draft.value().start_cmd.find("127.0.0.1") != std::string::npos);
  CHECK(draft.value().start_cmd.find("{port}") != std::string::npos);
  CHECK(berth::validate_recipe(draft.value()).is_ok());
}

TEST(detect_go_module_without_a_module_line_falls_back_to_the_name) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", "go 1.22\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().binary_name == "myapp");
}

TEST(detect_bun_lockfile_picks_bun) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "package.json",
                            R"({"name":"myapp","version":"0.0.0"})")
            .is_ok());
  CHECK(berth::atomic_write(tmp.path() / "bun.lockb", "binary-lock").is_ok());

  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().language == "node");
  CHECK(draft.value().tools == std::vector<std::string>{"bun"});
  CHECK(draft.value().build_steps.size() == 1);    // install only, no script
  CHECK(draft.value().binary_name == "index.js");  // package.json default
  CHECK(draft.value().start_cmd.find("{project}/index.js") !=
        std::string::npos);
  CHECK(berth::validate_recipe(draft.value()).is_ok());
}

TEST(detect_pnpm_lockfile_picks_pnpm) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "package.json",
                            R"({"name":"myapp","version":"0.0.0"})")
            .is_ok());
  CHECK(berth::atomic_write(tmp.path() / "pnpm-lock.yaml", "lock: v9").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().tools == std::vector<std::string>{"pnpm"});
}

TEST(detect_uses_package_json_scripts_and_main) {
  const TempDir tmp;
  CHECK(berth::atomic_write(
            tmp.path() / "package.json",
            R"({"name":"myapp","version":"0.0.0","main":"dist/server.js",)"
            R"("scripts":{"build":"tsc","start":"node dist/server.js"}})")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().build_steps.size() == 2);  // install + run build
  CHECK((draft.value().build_steps.back() ==
         std::vector<std::string>{"bun", "run", "build"}));
  CHECK(draft.value().binary_name == "dist/server.js");
  CHECK(draft.value().start_cmd.find("run start") != std::string::npos);
  CHECK(berth::validate_recipe(draft.value()).is_ok());
}

TEST(detect_refuses_an_entry_point_that_leaves_the_work_dir) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "package.json",
                            R"({"name":"myapp","main":"../escape.js"})")
            .is_ok());
  CHECK(berth::detect_recipe(request_for(tmp.path())).is_err());
}

TEST(detect_python_requirements_produce_a_valid_venv_draft) {
  const TempDir tmp;
  CHECK(
      berth::atomic_write(tmp.path() / "requirements.txt", "flask\n").is_ok());

  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().language == "python");
  CHECK(draft.value().tools == std::vector<std::string>{"python3"});
  CHECK(draft.value().binary_name == "venv/bin/python");
  CHECK(draft.value().build_steps.size() == 2);
  // The pip step runs through the venv interpreter; the tool-coverage
  // alias (python → python3) is what makes this validate (§8).
  CHECK(berth::validate_recipe(draft.value()).is_ok());
  CHECK(draft.value().start_cmd.find("{self} {project}/main.py") !=
        std::string::npos);
}

TEST(detect_python_pyproject_without_requirements_installs_the_project) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "pyproject.toml", "[project]\n")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().build_steps.back().size() >= 4);
  CHECK(draft.value().build_steps.back().back() == "{project}");
  CHECK(berth::validate_recipe(draft.value()).is_ok());
}

TEST(detect_go_wins_over_node_and_python) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(tmp.path() / "package.json", "{}").is_ok());
  CHECK(
      berth::atomic_write(tmp.path() / "requirements.txt", "flask\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().language == "go");
}

TEST(detect_dockerfile_expose_is_a_port_hint_only) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(
            tmp.path() / "Dockerfile",
            "FROM scratch\nEXPOSE 8080 9090\nENTRYPOINT [\"/myapp\"]\n")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().port == 8080);
}

TEST(detect_ignores_a_privileged_expose) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(tmp.path() / "Dockerfile", "EXPOSE 80\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().port == 0);  // privileged ports are unreachable
}

TEST(detect_refuses_a_tree_without_a_supported_stack) {
  const TempDir tmp;
  CHECK(
      berth::atomic_write(tmp.path() / "README.md", "nothing here\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_err());
  CHECK(draft.error().ctx().find("no supported stack") != std::string::npos);
}

TEST(detect_validates_the_coordinates_before_looking_at_files) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());

  DetectRequest bad_sha = request_for(tmp.path());
  bad_sha.commit = "main";
  CHECK(berth::detect_recipe(bad_sha).is_err());

  DetectRequest bad_repo = request_for(tmp.path());
  bad_repo.name = "../escape";
  CHECK(berth::detect_recipe(bad_repo).is_err());

  DetectRequest missing_dir = request_for(tmp.path() / "no-such-dir");
  CHECK(berth::detect_recipe(missing_dir).is_err());
}
