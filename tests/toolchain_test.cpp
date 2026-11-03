#include "berth/toolchain.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "berth/deploy.h"
#include "berth/recipes.h"
#include "core/atomic.h"
#include "core/paths.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::Recipe;
using berth::ToolchainPin;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  return p;
}

void put_executable(const std::filesystem::path& dir, const std::string& name) {
  CHECK(berth::ensure_dir(dir).is_ok());
  const std::filesystem::path file = dir / name;
  FILE* f = std::fopen(file.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("#!/bin/sh\nexit 0\n", f);
  std::fclose(f);
  CHECK(::chmod(file.c_str(), 0755) == 0);
}

Recipe bun_recipe() {
  Recipe r;
  r.binary_name = "server/server.js";
  r.start_cmd = "bun {project}/server/server.js --host=127.0.0.1 --port={port}";
  return r;
}

}

TEST(toolchain_pin_for_pnpm_on_x86_64) {
#if defined(__x86_64__)
  const std::optional<ToolchainPin> pin = berth::pinned_toolchain("pnpm");
  CHECK(pin.has_value());
  CHECK(pin->name == "pnpm");
  CHECK(pin->version == "11.7.0");
  CHECK(pin->sha256.size() == 64);
  CHECK(std::all_of(pin->sha256.begin(), pin->sha256.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }));
  CHECK(pin->inner_path == "pnpm");
  CHECK(pin->url.find("pnpm-linux-x64.tar.gz") != std::string::npos);
#endif
}

TEST(toolchain_unknown_tool_has_no_pin) {
  CHECK(!berth::pinned_toolchain("nope").has_value());
}

TEST(toolchain_version_marker_is_current_only_on_match) {
  const TempDir tmp;
  const std::filesystem::path marker = tmp.path() / "pnpm.version";
  CHECK(berth::atomic_write(marker, "11.7.0\n").is_ok());
  CHECK(berth::pin_is_current(marker, "11.7.0"));
  CHECK(!berth::pin_is_current(marker, "11.6.0"));
  CHECK(!berth::pin_is_current(tmp.path() / "absent.version", "11.7.0"));
}

TEST(toolchain_path_prepends_bin_dir) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const std::string bin = (p.state_dir / "toolbox" / "bin").string();
  CHECK(berth::path_with_toolbox(p, "/usr/bin:/bin") ==
        bin + ":/usr/bin:/bin");
  CHECK(berth::path_with_toolbox(p, "") == bin + ":");
}

TEST(toolchain_resolve_start_argv_ignores_empty_program_dir) {
  const TempDir tmp;
  put_executable(tmp.path() / "bin", "bun");
  const Recipe r = bun_recipe();

  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set((tmp.path() / "bin").string());

  std::string binary;
  const auto argv =
      berth::resolve_start_argv(r, tmp.path() / "wd", 3001, binary);
  CHECK(argv.is_ok());
  CHECK(binary == (tmp.path() / "bin" / "bun").string());
}

TEST(toolchain_resolve_start_argv_prefers_program_dir) {
  const TempDir tmp;
  const std::filesystem::path toolbox = tmp.path() / "toolbox" / "bin";
  put_executable(toolbox, "pnpm");
  Recipe r;
  r.binary_name = "lib/index.js";
  r.start_cmd = "pnpm {project}/lib/index.js --addr 127.0.0.1 --port {port}";

  std::string binary;
  const auto argv =
      berth::resolve_start_argv(r, tmp.path() / "wd", 3001, binary, toolbox);
  CHECK(argv.is_ok());
  CHECK(binary == (toolbox / "pnpm").string());
  CHECK(argv.value().front() ==
        (tmp.path() / "wd" / "lib" / "index.js").string());
}
