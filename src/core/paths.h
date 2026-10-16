#pragma once

// paths.cpp is the SINGLE source of truth for every path: no module builds an
// XDG path itself. An empty environment variable means unset (§11).

#include <filesystem>
#include <string>

#include "util/result.h"

namespace berth {

struct Paths {
  std::filesystem::path state_dir;
  std::filesystem::path data_dir;
  std::filesystem::path services_dir;  // one dir per deploy
  std::filesystem::path config_dir;
  std::filesystem::path recipes_dir;   // user recipes
  std::filesystem::path stacks_dir;    // multi-unit user recipes (§8, v2)
  std::filesystem::path units_dir;
  std::filesystem::path secrets_dir;   // 0600 files
  std::filesystem::path state_json;
  std::filesystem::path deploy_journal;
  std::filesystem::path deploy_lock;
  std::filesystem::path net_log;
  std::filesystem::path deploy_log;
  std::filesystem::path crash_log;
};

[[nodiscard]] Result<Paths> paths_from_env();

}
