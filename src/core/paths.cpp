// src/core/paths.cpp — see paths.h for the contract.

#include "paths.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/names.h"
#include "util/env.h"

namespace berth {

namespace {

// A relative XDG_* value is ignored, not honoured: honouring it would let a
// stray export aim the state dir at the process CWD.
std::filesystem::path xdg_root(const std::filesystem::path& home,
                               const char* env_name,
                               std::string_view fallback_suffix) {
  const std::optional<std::string> value = env_nonempty(env_name);
  if (value.has_value()) {
    const std::filesystem::path given(*value);
    if (given.is_absolute())
      return given;
  }
  return home / std::filesystem::path(fallback_suffix);
}

std::filesystem::path app_dir(const std::filesystem::path& root) {
  return root / std::filesystem::path(kAppName);
}

}  // namespace

Result<Paths> paths_from_env() {
  const std::optional<std::string> home = env_nonempty("HOME");
  if (!home.has_value()) {
    return Error("paths: HOME is unset or empty");
  }
  const std::filesystem::path home_dir(*home);

  Paths out;
  out.state_dir = app_dir(xdg_root(home_dir, "XDG_STATE_HOME", ".local/state"));
  out.data_dir = app_dir(xdg_root(home_dir, "XDG_DATA_HOME", ".local/share"));
  out.config_dir = app_dir(xdg_root(home_dir, "XDG_CONFIG_HOME", ".config"));
  out.units_dir =
      xdg_root(home_dir, "XDG_CONFIG_HOME", ".config") / "systemd" / "user";
  out.recipes_dir = out.config_dir / "recipes";
  out.services_dir = out.data_dir / "services";
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
