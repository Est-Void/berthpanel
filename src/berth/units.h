#pragma once

// src/berth/units.h — systemd user unit rendering, hardening, value
// escaping, and the systemctl calls (dev.md §5, §4).
//
// Two traps live here and are the reason this module exists (§11):
//   * Quoting is done PER ARGUMENT and never by splitting a command line
//     on whitespace. A $HOME containing a space used to truncate the
//     exec'd path and produce a start-limit error pointing nowhere.
//   * List-valued directives (ReadWritePaths=, InaccessiblePaths=) are
//     emitted as ONE DIRECTIVE PER PATH. Joining them with spaces turns
//     one path into two nonexistent ones and the namespace setup fails.
//   * '%' is doubled for systemd specifiers; newline/NUL in any value is
//     a loud error, because a unit file cannot represent it.
// Hardening below reduces blast radius — it is not a security boundary,
// and nothing here may be read as a guarantee. The build sandbox has its
// own statement to that effect (sandbox.cpp, stage 4.14).
//
// The unit name is validated with the ONE shared validator from state.h
// (§11: a creator that sanitises and a remover that does not disagree).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../paths.h"
#include "../util/result.h"

namespace berth {

struct UnitLimits {
  std::string memory_max;              // e.g. "384M"
  std::uint32_t tasks_max = 0;         // e.g. 300
  bool empty() const { return memory_max.empty() || tasks_max == 0; }
};

struct UnitSpec {
  std::string service_name;            // unit stem, must pass validate_unit_name
  std::string description;
  std::filesystem::path working_dir;
  std::filesystem::path secrets_file;  // EnvironmentFile=, may be empty
  std::string exec_binary;             // absolute path (or resolved by caller)
  std::vector<std::string> exec_args;  // already-substituted argv tail
  UnitLimits limits;                   // MANDATORY (§8)
  std::vector<std::filesystem::path> read_write_paths;
  std::vector<std::string> extra_environment;  // "KEY=VALUE", quoted per arg
};

// Quotes one ExecStart word (systemd syntax): empty becomes "", every
// argument is quoted, '"' and '\' are escaped, '%' doubled.
[[nodiscard]] Result<std::string> quote_exec_word(std::string_view arg);

// Quotes one directive VALUE that is not a word list (paths, Environment=).
[[nodiscard]] Result<std::string> quote_unit_value(std::string_view value);

// Joins argv into an ExecStart= value with per-argument quoting.
[[nodiscard]] Result<std::string> render_exec_start(
    std::string_view binary, const std::vector<std::string>& args);

// {bin} {venv} {python} {project} {port} {self} substitution for recipe
// start commands. Unknown placeholders are a loud error.
[[nodiscard]] Result<std::string> substitute_placeholders(
    std::string_view tmpl, const std::vector<std::pair<std::string, std::string>>& values);

// Full unit text. Err when the name is invalid or the resource limits are
// missing — a unit without them is exactly what §8 forbids.
[[nodiscard]] Result<std::string> render_unit(const UnitSpec& spec);

// Writes the unit atomically (0644) into $XDG_CONFIG_HOME/systemd/user.
[[nodiscard]] Result<std::filesystem::path> write_unit(const Paths& paths,
                                                      const UnitSpec& spec);

// systemctl --user with an argv array (absolute binary, 20s timeout).
[[nodiscard]] Result<void> systemctl_user(const std::vector<std::string>& args);
[[nodiscard]] Result<void> systemctl_daemon_reload();
[[nodiscard]] Result<void> systemctl_enable_now(const std::string& service_name);
[[nodiscard]] Result<void> systemctl_disable(const std::string& service_name);

}  // namespace berth
