// Tests for detect.h (stage 4.20): stack heuristics → recipe DRAFT.
// Detection only reads fixture trees, never executes anything. Several
// stack families at once is a loud Err, npm lockfiles are refused (not
// guessed), and every draft must pass validate_recipe as-is — quick-accept
// (4.21) must be honest — so each test asserts the validator agrees with
// the heuristic.

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
  CHECK(draft.value().recipe.language == "go");
  CHECK(draft.value().recipe.tools == std::vector<std::string>{"go"});
  CHECK(draft.value().recipe.binary_name == "myapp");  // module path basename
  CHECK(draft.value().recipe.build_steps.size() == 1);
  CHECK(
      (draft.value().recipe.build_steps.front() ==
       std::vector<std::string>{"go", "build", "-o", "{project}/myapp", "."}));
  CHECK(draft.value().recipe.start_cmd.find("127.0.0.1") != std::string::npos);
  CHECK(draft.value().recipe.start_cmd.find("{port}") != std::string::npos);
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
}

TEST(detect_go_module_without_a_module_line_falls_back_to_the_name) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", "go 1.22\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().recipe.binary_name == "myapp");
}

TEST(detect_bun_lockfile_picks_bun) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "package.json",
                            R"({"name":"myapp","version":"0.0.0"})")
            .is_ok());
  CHECK(berth::atomic_write(tmp.path() / "bun.lockb", "binary-lock").is_ok());

  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().recipe.language == "node");
  CHECK(draft.value().recipe.tools == std::vector<std::string>{"bun"});
  CHECK(draft.value().recipe.build_steps.size() == 1);    // install only, no script
  CHECK(draft.value().recipe.binary_name == "index.js");  // package.json default
  CHECK(draft.value().recipe.start_cmd.find("{project}/index.js") !=
        std::string::npos);
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
}

TEST(detect_pnpm_lockfile_picks_pnpm) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "package.json",
                            R"({"name":"myapp","version":"0.0.0"})")
            .is_ok());
  CHECK(berth::atomic_write(tmp.path() / "pnpm-lock.yaml", "lock: v9").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().recipe.tools == std::vector<std::string>{"pnpm"});
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
  CHECK(draft.value().recipe.build_steps.size() == 2);  // install + run build
  CHECK((draft.value().recipe.build_steps.back() ==
         std::vector<std::string>{"bun", "run", "build"}));
  CHECK(draft.value().recipe.binary_name == "dist/server.js");
  CHECK(draft.value().recipe.start_cmd.find("run start") != std::string::npos);
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
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
  CHECK(draft.value().recipe.language == "python");
  CHECK(draft.value().recipe.tools == std::vector<std::string>{"python3"});
  CHECK(draft.value().recipe.binary_name == "venv/bin/python");
  CHECK(draft.value().recipe.build_steps.size() == 2);
  // The pip step runs through the venv interpreter; the tool-coverage
  // alias (python → python3) is what makes this validate (§8).
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
  CHECK(draft.value().recipe.start_cmd.find("{self} {project}/main.py") !=
        std::string::npos);
}

TEST(detect_python_pyproject_without_requirements_installs_the_project) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "pyproject.toml", "[project]\n")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().recipe.build_steps.back().size() >= 4);
  CHECK(draft.value().recipe.build_steps.back().back() == "{project}");
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
}

TEST(detect_refuses_an_ambiguous_stack) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(tmp.path() / "package.json", "{}").is_ok());
  CHECK(
      berth::atomic_write(tmp.path() / "requirements.txt", "flask\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_err());
  // The heuristic must not silently pick a family; the message names the
  // markers so the user can disambiguate (plan 4.20).
  CHECK(draft.error().ctx().find("ambiguous stack") != std::string::npos);
  CHECK(draft.error().ctx().find("go.mod + package.json") != std::string::npos);
}

TEST(detect_refuses_an_npm_lockfile) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "package.json",
                            R"({"name":"myapp","version":"0.0.0"})")
            .is_ok());
  CHECK(berth::atomic_write(tmp.path() / "package-lock.json", "{}").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_err());
  // Not a silent bun guess: npm is not in the v1 toolchain, and building
  // the tree with the wrong package manager is not our decision to make.
  CHECK(draft.error().ctx().find("npm is not in the v1 toolchain") !=
        std::string::npos);
}

TEST(detect_prefetch_steps_download_without_executing) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  const auto go = berth::detect_recipe(request_for(tmp.path()));
  CHECK(go.is_ok());
  CHECK((go.value().recipe.prefetch_steps.front() ==
         std::vector<std::string>{"go", "mod", "download"}));

  const TempDir tmp2;
  CHECK(berth::atomic_write(tmp2.path() / "package.json",
                            R"({"name":"myapp","version":"0.0.0"})")
            .is_ok());
  CHECK(berth::atomic_write(tmp2.path() / "bun.lockb", "binary-lock").is_ok());
  const auto bun = berth::detect_recipe(request_for(tmp2.path()));
  CHECK(bun.is_ok());
  // §6.5: the host downloads dependencies, it never runs their scripts.
  CHECK((bun.value().recipe.prefetch_steps.front() ==
         std::vector<std::string>{"bun", "install", "--ignore-scripts"}));
}

TEST(detect_cmd_exec_form_hints_loopback_flags) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(
            tmp.path() / "Dockerfile",
            "FROM scratch\nCMD [\"/myapp\", \"-text\", \"hi\", "
            "\"-addr\", \"127.0.0.1\"]\n")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  // argv[0] is replaced by {self}; the rest becomes the flag section, and
  // {port} is appended so the deploy can substitute the picked port.
  CHECK(draft.value().recipe.start_cmd ==
        "{self} -text hi -addr 127.0.0.1 --port {port}");
  bool mentions_cmd = false;
  for (const std::string& warning : draft.value().warnings) {
    if (warning.find("Dockerfile CMD") != std::string::npos)
      mentions_cmd = true;
  }
  CHECK(mentions_cmd);
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
}

TEST(detect_cmd_without_a_loopback_bind_falls_back_to_the_template) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(tmp.path() / "Dockerfile",
                            "FROM scratch\nCMD [\"/myapp\", \"-text\", \"hi\"]\n")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().recipe.start_cmd.find("{port}") != std::string::npos);
  CHECK(draft.value().recipe.start_cmd.find("127.0.0.1") !=
        std::string::npos);
}

TEST(detect_cmd_shell_syntax_is_a_warning_not_a_draft) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(tmp.path() / "Dockerfile",
                            "FROM scratch\nCMD server -p $(cat port)\n")
            .is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());  // the draft survives; the hint is refused loudly
  CHECK(draft.value().recipe.start_cmd.find("{port}") != std::string::npos);
  bool mentions_shell = false;
  for (const std::string& warning : draft.value().warnings) {
    if (warning.find("shell syntax") != std::string::npos)
      mentions_shell = true;
  }
  CHECK(mentions_shell);
  CHECK(berth::validate_recipe(draft.value().recipe).is_ok());
}

TEST(detect_every_draft_carries_the_start_cmd_assumption_warning) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  bool assumption = false;
  for (const std::string& warning : draft.value().warnings) {
    if (warning.find("start_cmd is an assumption") != std::string::npos)
      assumption = true;
  }
  CHECK(assumption);
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
  CHECK(draft.value().recipe.port == 8080);
}

TEST(detect_ignores_a_privileged_expose) {
  const TempDir tmp;
  CHECK(berth::atomic_write(tmp.path() / "go.mod", kGoMod).is_ok());
  CHECK(berth::atomic_write(tmp.path() / "Dockerfile", "EXPOSE 80\n").is_ok());
  const auto draft = berth::detect_recipe(request_for(tmp.path()));
  CHECK(draft.is_ok());
  CHECK(draft.value().recipe.port == 0);  // privileged ports are unreachable
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
