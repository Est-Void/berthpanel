// Tests for paths.h (stage 2.1). Every case mutates the environment, so
// each runs under the global env_test_lock (dev.md §9) and restores what
// it touched via ScopedEnv.

#include "core/paths.h"

#include <mutex>

#include "runner.h"
#include "test_env.h"
#include "util/env.h"

using berth::paths_from_env;

TEST(paths_defaults_from_home) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv home("HOME");
  home.set("/home/u");
  ScopedEnv state("XDG_STATE_HOME");
  state.unset();
  ScopedEnv data("XDG_DATA_HOME");
  data.unset();
  ScopedEnv config("XDG_CONFIG_HOME");
  config.unset();

  const auto r = paths_from_env();
  CHECK(r.is_ok());
  CHECK(r.value().state_dir == "/home/u/.local/state/berthpanel");
  CHECK(r.value().data_dir == "/home/u/.local/share/berthpanel");
  CHECK(r.value().services_dir == "/home/u/.local/share/berthpanel/services");
  CHECK(r.value().config_dir == "/home/u/.config/berthpanel");
  CHECK(r.value().recipes_dir == "/home/u/.config/berthpanel/recipes");
  CHECK(r.value().stacks_dir == "/home/u/.config/berthpanel/stacks");
  CHECK(r.value().secrets_dir == "/home/u/.local/state/berthpanel/secrets");
  CHECK(r.value().state_json == "/home/u/.local/state/berthpanel/state.json");
  CHECK(r.value().deploy_journal ==
        "/home/u/.local/state/berthpanel/deploy-journal.json");
  CHECK(r.value().deploy_lock == "/home/u/.local/state/berthpanel/deploy.lock");
  CHECK(r.value().net_log == "/home/u/.local/state/berthpanel/net.log");
  CHECK(r.value().deploy_log == "/home/u/.local/state/berthpanel/deploy.log");
  CHECK(r.value().crash_log == "/home/u/.local/state/berthpanel/crash.log");
}

TEST(paths_xdg_overrides_apply) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv home("HOME");
  home.set("/home/u");
  ScopedEnv state("XDG_STATE_HOME");
  state.set("/abs/state");
  ScopedEnv data("XDG_DATA_HOME");
  data.set("/abs/data");
  ScopedEnv config("XDG_CONFIG_HOME");
  config.set("/abs/config");

  const auto r = paths_from_env();
  CHECK(r.is_ok());
  CHECK(r.value().state_dir == "/abs/state/berthpanel");
  CHECK(r.value().services_dir == "/abs/data/berthpanel/services");
  CHECK(r.value().config_dir == "/abs/config/berthpanel");
}

TEST(paths_empty_env_is_unset) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv home("HOME");
  home.set("/home/u");
  ScopedEnv state("XDG_STATE_HOME");
  state.set("");  // the §11 trap: empty means unset, never relative
  ScopedEnv data("XDG_DATA_HOME");
  data.unset();
  ScopedEnv config("XDG_CONFIG_HOME");
  config.unset();

  const auto r = paths_from_env();
  CHECK(r.is_ok());
  CHECK(r.value().state_dir == "/home/u/.local/state/berthpanel");
}

TEST(paths_relative_xdg_ignored) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv home("HOME");
  home.set("/home/u");
  ScopedEnv data("XDG_DATA_HOME");
  data.set("relative/path");  // XDG spec: non-absolute is invalid
  ScopedEnv state("XDG_STATE_HOME");
  state.unset();
  ScopedEnv config("XDG_CONFIG_HOME");
  config.unset();

  const auto r = paths_from_env();
  CHECK(r.is_ok());
  CHECK(r.value().services_dir == "/home/u/.local/share/berthpanel/services");
}

TEST(paths_empty_home_is_error) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv home("HOME");
  home.set("");

  const auto r = paths_from_env();
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("HOME") != std::string::npos);
}

TEST(paths_unset_home_is_error) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv home("HOME");
  home.unset();

  const auto r = paths_from_env();
  CHECK(r.is_err());
}
