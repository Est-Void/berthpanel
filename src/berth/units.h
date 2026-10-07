#pragma once

// src/berth/units.h — systemd user unit rendering, value escaping, and the
// systemctl client (dev.md §5, §4). The unit NAME policy is core/names.h's;
// this module never invents a name.
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
//
// Hardening reduces blast radius — it is not a security boundary, and
// nothing here may be read as a guarantee. The build sandbox says so in
// its own units too (sandbox.cpp), and it renders the SAME hardening set
// from hardening_directives(): a change to one and not the other would leave
// a build less contained than the service it produces.

#include <array>
#include <atomic>
#include <span>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

// The hardening set, as data, so the unit file and the build sandbox cannot
// drift apart.
[[nodiscard]] std::span<const std::string_view> hardening_directives();

struct UnitLimits {
  std::string memory_max;       // e.g. "384M"
  std::uint32_t tasks_max = 0;  // e.g. 300
  bool empty() const { return memory_max.empty() || tasks_max == 0; }
};

struct UnitSpec {
  std::string service_name;  // unit stem, must pass validate_unit_name
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

// Quotes a directive value that systemd parses like a shell word
// (Environment=K=V, Environment=).
[[nodiscard]] Result<std::string> quote_unit_value(std::string_view value);

// Path directives (WorkingDirectory=, ReadWritePaths=, EnvironmentFile=)
// take the RAW path: systemd parses the rest of the line verbatim, so
// quotes would become part of the path (verified against a live user
// manager: "WorkingDirectory=/tmp/with space" starts fine). Spaces are
// therefore safe; quoting is not. ExecStart is the opposite case and is
// quoted per argument. Getting either one wrong is the same class of bug
// as the whitespace-splitting one (dev.md §11).
[[nodiscard]] Result<std::string> raw_systemd_path(
    const std::filesystem::path& path);

// Joins argv into an ExecStart= value with per-argument quoting.
[[nodiscard]] Result<std::string> render_exec_start(
    std::string_view binary, const std::vector<std::string>& args);

// {bin} {venv} {python} {project} {port} {self} substitution for recipe
// start commands. Unknown placeholders are a loud error.
[[nodiscard]] Result<std::string> substitute_placeholders(
    std::string_view tmpl,
    const std::vector<std::pair<std::string, std::string>>& values);

// Full unit text. Err when the name is invalid or the resource limits are
// missing — a unit without them is exactly what §8 forbids.
[[nodiscard]] Result<std::string> render_unit(const UnitSpec& spec);

// Writes the unit atomically (0644) into $XDG_CONFIG_HOME/systemd/user.
[[nodiscard]] Result<std::filesystem::path> write_unit(const Paths& paths,
                                                       const UnitSpec& spec);

// systemctl --user with an argv array (absolute binary, 20s timeout).
[[nodiscard]] Result<void> systemctl_user(const std::vector<std::string>& args);

// systemctl --user with an explicit budget and cancellation, returning
// the child's exit code as a VALUE: systemctl exit codes are data (e.g.
// `is-active` exits nonzero for "not active" without being broken). Err
// only on spawn/wait/timeout/cancellation.
[[nodiscard]] Result<int> systemctl_user_code(
    const std::vector<std::string>& args, std::chrono::milliseconds timeout,
    std::atomic<bool>* cancel);
[[nodiscard]] Result<void> systemctl_daemon_reload();
[[nodiscard]] Result<void> systemctl_enable_now(
    const std::string& service_name);
[[nodiscard]] Result<void> systemctl_disable(const std::string& service_name);

// --- stage 4.18: the interrupted-deploy cleanup units --------------------

struct CleanupUnitTexts {
  std::string service;  // berthpanel-cleanup.service text
  std::string timer;    // berthpanel-cleanup.timer text
};

// Pure renderer. The service is a oneshot running `<binary> __cleanup` as
// the user; the timer fires it daily and after boot (Persistent=true: a
// machine that was off when a deploy died still gets its sweep).
[[nodiscard]] Result<CleanupUnitTexts> render_cleanup_units(
    const std::filesystem::path& binary_path);

// Renders and writes both unit files into units_dir. Does NOT reload or
// enable — installing is a systemctl decision the CLI composes, so this
// stays testable without a live user manager.
[[nodiscard]] Result<std::filesystem::path> write_cleanup_units(
    const Paths& paths, const std::filesystem::path& binary_path);

}  // namespace berth
