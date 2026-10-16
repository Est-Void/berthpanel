// Tests for util/which.h: PATH resolution against a fake PATH, under the
// global env_test_lock (dev.md §9).

#include "util/which.h"

#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>

#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::resolve_executable;

namespace {

void put_executable(const std::filesystem::path& dir, const std::string& name) {
  CHECK(berth::ensure_dir(dir).is_ok());
  const std::filesystem::path file = dir / name;
  FILE* f = std::fopen(file.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("#!/bin/sh\nexit 0\n", f);
  std::fclose(f);
  CHECK(::chmod(file.c_str(), 0755) == 0);
}

}
TEST(which_finds_executable_on_path) {
  const TempDir tmp;
  put_executable(tmp.path() / "bin", "faketool");

  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set((tmp.path() / "bin").string());

  const auto found = resolve_executable("faketool");
  CHECK(found.is_ok());
  CHECK(found.value() == (tmp.path() / "bin" / "faketool").string());
}

TEST(which_reports_missing_and_malformed) {
  const TempDir tmp;
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set(tmp.path().string());

  CHECK(resolve_executable("absent-tool").is_err());
  CHECK(resolve_executable("").is_err());
  CHECK(resolve_executable("/bin/sh").is_err());  // no path separator allowed
}

TEST(which_rejects_empty_path) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set("");  // empty means unset (§11)
  CHECK(resolve_executable("anything").is_err());
}

TEST(which_skips_non_executable_files) {
  const TempDir tmp;
  CHECK(berth::ensure_dir(tmp.path() / "bin").is_ok());
  FILE* f = std::fopen((tmp.path() / "bin" / "notexec").string().c_str(), "w");
  CHECK(f != nullptr);
  std::fclose(f);
  CHECK(::chmod((tmp.path() / "bin" / "notexec").c_str(), 0644) == 0);

  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set((tmp.path() / "bin").string());
  CHECK(resolve_executable("notexec").is_err());
}
