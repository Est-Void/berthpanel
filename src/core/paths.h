#pragma once

// src/core/paths.h — the SINGLE source of truth for where berthpanel keeps
// things (dev.md §3). Every module takes a Paths by const reference and
// derives its own filenames from it; nothing builds an XDG path itself. An
// empty environment variable means unset (§11): an exported-but-empty
// XDG_STATE_HOME must fall back to the default, not to the process CWD.

#include <filesystem>
#include <string>

#include "util/result.h"

namespace berth {

struct Paths {
  std::filesystem::path state_dir;     // XDG_STATE_HOME/berthpanel
  std::filesystem::path data_dir;      // XDG_DATA_HOME/berthpanel
  std::filesystem::path services_dir;  // data_dir/services, one dir per deploy
  std::filesystem::path config_dir;    // XDG_CONFIG_HOME/berthpanel
  std::filesystem::path recipes_dir;   // config_dir/recipes, user recipes
  std::filesystem::path units_dir;     // XDG_CONFIG_HOME/systemd/user
  std::filesystem::path secrets_dir;   // state_dir/secrets, 0600 files
  std::filesystem::path state_json;
  std::filesystem::path deploy_journal;
  std::filesystem::path deploy_lock;
  std::filesystem::path net_log;
  std::filesystem::path deploy_log;
  std::filesystem::path crash_log;
};

[[nodiscard]] Result<Paths> paths_from_env();

}  // namespace berth
