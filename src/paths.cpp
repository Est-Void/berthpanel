#include "paths.h"
#include <filesystem>
#include "util/env.h"

namespace berth {

Result<Paths> paths_from_env() {
  const std::optional<std::string> home = env_nonempty("HOME");
  if (!home.has_value()) {
    return Error("paths: HOME is unset or empty");
  }
  const std::filesystem::path home_dir(*home);

  const auto xdg_root =
      [&](const char* name,
          std::string_view fallback_suffix) -> std::filesystem::path {
    const std::optional<std::string> value = env_nonempty(name);
    if (value.has_value()) {
      const std::filesystem::path given(*value);
      if (given.is_absolute()) return given;
    }
    return home_dir / std::filesystem::path(fallback_suffix);
  };

  Paths out;
  out.state_dir = xdg_root("XDG_STATE_HOME", ".local/state") / "berthpanel";
  out.data_dir = xdg_root("XDG_DATA_HOME", ".local/share") / "berthpanel";
  out.config_dir = xdg_root("XDG_CONFIG_HOME", ".config") / "berthpanel";
  out.services_dir = out.data_dir / "services";
  out.units_dir = xdg_root("XDG_CONFIG_HOME", ".config") / "systemd" / "user";
  out.secrets_dir = out.state_dir / "secrets";
  out.state_json = out.state_dir / "state.json";
  out.deploy_journal = out.state_dir / "deploy-journal.json";
  out.deploy_lock = out.state_dir / "deploy.lock";
  out.net_log = out.state_dir / "net.log";
  out.deploy_log = out.state_dir / "deploy.log";
  out.crash_log = out.state_dir / "crash.log";
  return out;
}

}  // namespace berth
