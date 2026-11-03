#include "berth/buildenv.h"

#include <filesystem>
#include <string>
#include <vector>

#include "runner.h"

namespace {

std::vector<std::string> value_prefixes(
    const std::vector<std::string>& entries) {
  std::vector<std::string> keys;
  for (const std::string& entry : entries) {
    keys.push_back(entry.substr(0, entry.find('=')));
  }
  return keys;
}

}

TEST(build_environment_lists_every_toolchain_cache_in_fixed_order) {
  const std::vector<std::string> entries =
      berth::build_environment("/tmp/svc");
  CHECK(value_prefixes(entries) ==
        std::vector<std::string>({"HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME",
                                  "GOCACHE", "GOPATH", "GOMODCACHE",
                                  "CARGO_HOME", "NPM_CONFIG_CACHE",
                                  "NPM_CONFIG_STORE_DIR"}));
}

TEST(build_environment_entries_are_non_empty_key_value_pairs) {
  for (const std::string& entry : berth::build_environment("/tmp/svc")) {
    const std::size_t eq = entry.find('=');
    CHECK(eq != std::string::npos);
    CHECK(eq > 0);
    CHECK(!entry.substr(eq + 1).empty());
  }
}

TEST(build_environment_paths_live_under_the_work_dir) {
  const std::filesystem::path work_dir = "/tmp/svc";
  const std::string prefix = work_dir.string() + "/";
  for (const std::string& entry : berth::build_environment(work_dir)) {
    const std::string value = entry.substr(entry.find('=') + 1);
    CHECK(value.rfind(prefix, 0) == 0);
  }
}

TEST(build_environment_preserves_spaces_without_quoting) {
  const std::vector<std::string> entries =
      berth::build_environment("/tmp/two words/svc");
  for (const std::string& entry : entries) {
    CHECK(entry.find("two words") != std::string::npos);
    CHECK(entry.find('"') == std::string::npos);
    CHECK(entry.find('\'') == std::string::npos);
  }
}

TEST(build_environment_is_deterministic) {
  const std::filesystem::path work_dir = "/tmp/svc";
  CHECK(berth::build_environment(work_dir) ==
        berth::build_environment(work_dir));
}
