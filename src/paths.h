#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "util/result.h"

namespace berth {

struct Paths {
  std::filesystem::path state_dir;     
  std::filesystem::path data_dir;      
  std::filesystem::path services_dir;  
  std::filesystem::path config_dir;      
  std::filesystem::path units_dir;      // $XDG_CONFIG_HOME/systemd/user
  std::filesystem::path secrets_dir;   
  std::filesystem::path state_json;    
  std::filesystem::path deploy_journal;  
  std::filesystem::path deploy_lock;     
  std::filesystem::path net_log;         
  std::filesystem::path deploy_log;      
  std::filesystem::path crash_log;      
};

[[nodiscard]] Result<Paths> paths_from_env();

}  // namespace berth
