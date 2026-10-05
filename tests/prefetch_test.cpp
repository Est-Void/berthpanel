#include "berth/prefetch.h"

#include <string>
#include <vector>

#include "runner.h"

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
  const std::vector<std::vector<std::string>> steps = {
      {"bun", "install"},  // missing --ignore-scripts
      {"go", "mod", "download"},
  };
  const auto ran =
      run_prefetch(steps, nullptr, std::chrono::milliseconds(1000));
  CHECK(ran.is_err());
  CHECK(ran.error().ctx().find("ignore-scripts") != std::string::npos);
}