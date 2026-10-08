#include "berth/prefetch.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"
#include "util/which.h"

using berth::run_prefetch;
using berth::validate_prefetch_step;

TEST(prefetch_allows_only_download_only_steps) {
  CHECK(validate_prefetch_step({"go", "mod", "download"}).is_ok());
  CHECK(validate_prefetch_step(
            {"python3", "-m", "pip", "download", "-r", "requirements.txt"})
            .is_ok());
  CHECK(validate_prefetch_step(
            {"bun", "install", "--frozen-lockfile", "--ignore-scripts"})
            .is_ok());
  CHECK(validate_prefetch_step({"pnpm", "fetch", "--ignore-scripts"}).is_ok());
  CHECK(validate_prefetch_step({"npm", "ci", "--ignore-scripts"}).is_ok());
}

TEST(prefetch_refuses_install_scripts_on_the_host) {
  // A postinstall hook must never run outside the sandbox (§6.5).
  CHECK(
      validate_prefetch_step({"bun", "install", "--frozen-lockfile"}).is_err());
  CHECK(validate_prefetch_step({"npm", "install"}).is_err());
  CHECK(validate_prefetch_step({"pnpm", "install"}).is_err());
  CHECK(validate_prefetch_step({"python3", "-m", "pip", "install", "flask"})
            .is_err());
}

TEST(prefetch_refuses_arbitrary_programs_and_builds) {
  CHECK(validate_prefetch_step({}).is_err());
  CHECK(validate_prefetch_step({"curl", "-o", "x", "https://example.com"})
            .is_err());
  CHECK(validate_prefetch_step({"bash", "setup.sh"}).is_err());
  CHECK(validate_prefetch_step({"go", "build", "."}).is_err());
  CHECK(validate_prefetch_step({"go", "run", "main.go"}).is_err());
  CHECK(validate_prefetch_step({"sh", "-c", "rm -rf /"}).is_err());
}

TEST(prefetch_run_stops_at_the_first_bad_step) {
  const TempDir tmp;
  const std::vector<std::vector<std::string>> steps = {
      {"bun", "install"},  // missing --ignore-scripts
      {"go", "mod", "download"},
  };
  const auto ran = run_prefetch(steps, tmp.path(), nullptr,
                                std::chrono::milliseconds(1000));
  CHECK(ran.is_err());
  CHECK(ran.error().ctx().find("ignore-scripts") != std::string::npos);
}

TEST(prefetch_steps_run_IN_the_checkout) {
  // The bug this pins: `go mod download` was run in whatever directory the
  // panel happened to start in, found no go.mod there, and answered "no
  // modules specified" — a message about the USER's project that was really
  // about ours. A step that reads a manifest must read it where the
  // manifest is.
  const TempDir tmp;
  const std::filesystem::path project = tmp.path() / "checkout";
  CHECK(berth::ensure_dir(project).is_ok());
  // A real, tiny Go module. `go` may be absent on a test machine — then the
  // step cannot run, and the only thing this test asserts is the DIRECTORY.
  CHECK(berth::atomic_write(project / "go.mod",
                            "module example.com/probe\n\ngo 1.21\n")
            .is_ok());

  const std::vector<std::vector<std::string>> steps = {
      {"go", "mod", "download"},
  };
  const auto ran = run_prefetch(steps, project, nullptr,
                                std::chrono::milliseconds(30000));
  if (const auto go = berth::resolve_executable("go"); go.is_ok()) {
    // With a go.mod under the working directory, `go mod download` has
    // something to download and does NOT answer "no modules specified".
    if (ran.is_err()) {
      CHECK(ran.error().ctx().find("no modules specified") ==
            std::string::npos);
    }
  }
  // A working directory that does not exist is refused by NAME, before the
  // child runs: silence there would be a mystery error later.
  const auto missing = run_prefetch(steps, project / "nope", nullptr,
                                    std::chrono::milliseconds(1000));
  CHECK(missing.is_err());
  CHECK(missing.error().ctx().find("nope") != std::string::npos);
}