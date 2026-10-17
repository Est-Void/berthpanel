#pragma once

// Traps (dev.md §11): quoting is PER ARGUMENT, never a whitespace split; a list
// directive is ONE LINE PER PATH (joining invents nonexistent paths); '%' is
// doubled and a newline/NUL is a loud error.

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
#include "../util/subprocess.h"

namespace berth {

[[nodiscard]] std::span<const std::string_view> hardening_directives();

struct UnitLimits {
  std::string memory_max;
  std::uint32_t tasks_max = 0;
  bool empty() const { return memory_max.empty() || tasks_max == 0; }
};

struct UnitSpec {
  std::string service_name;
  std::string description;
  std::filesystem::path working_dir;
  std::filesystem::path secrets_file;
  std::string exec_binary;  // absolute path (or resolved by caller)
  std::vector<std::string> exec_args;
  UnitLimits limits;  // MANDATORY (§8): render_unit errs without them
  std::vector<std::filesystem::path> read_write_paths;
  std::vector<std::string> extra_environment;  // "KEY=VALUE", quoted per arg
  // Stack ordering: sibling unit STEMS (already berthpanel- prefixed). Rendered
  // as After=/Wants= and re-validated in render_unit; a raw member id must
  // never reach here unvalidated.
  std::vector<std::string> after_units;
  std::vector<std::string> wants_units;
};

// ExecStart word syntax: empty becomes "", '"' and '\' escaped, '%' doubled.
[[nodiscard]] Result<std::string> quote_exec_word(std::string_view arg);

// For directives systemd parses like a shell word (Environment=K=V).
[[nodiscard]] Result<std::string> quote_unit_value(std::string_view value);

// Single-path directives take the RAW path: quotes would become part of it.
[[nodiscard]] Result<std::string> raw_systemd_path(
    const std::filesystem::path& path);

// List directives split on unquoted whitespace, so a space must be
// backslash-escaped or the tail is silently ignored.
[[nodiscard]] std::string escape_path_list_value(std::string_view value);

[[nodiscard]] Result<std::string> render_exec_start(
    std::string_view binary, const std::vector<std::string>& args);

[[nodiscard]] Result<std::string> substitute_placeholders(
    std::string_view tmpl,
    const std::vector<std::pair<std::string, std::string>>& values);

[[nodiscard]] Result<std::string> render_unit(const UnitSpec& spec);

[[nodiscard]] Result<std::filesystem::path> write_unit(const Paths& paths,
                                                       const UnitSpec& spec);

[[nodiscard]] Result<void> systemctl_user(const std::vector<std::string>& args);

// The exit code is a VALUE: systemctl's codes are data, not Err.
[[nodiscard]] Result<int> systemctl_user_code(
    const std::vector<std::string>& args, std::chrono::milliseconds timeout,
    std::atomic<bool>* cancel);

// Needed where the exit code cannot tell states apart: `is-active` exits
// nonzero for BOTH "inactive" and "activating" (crash-loop auto-restart).
[[nodiscard]] Result<RunOutput> systemctl_user_output(
    const std::vector<std::string>& args, std::chrono::milliseconds timeout,
    std::atomic<bool>* cancel);
[[nodiscard]] Result<void> systemctl_daemon_reload();
[[nodiscard]] Result<void> systemctl_enable_now(
    const std::string& service_name);
[[nodiscard]] Result<void> systemctl_disable(const std::string& service_name);

struct CleanupUnitTexts {
  std::string service;
  std::string timer;
};

// Persistent=true: a machine off when a deploy died still gets its sweep.
[[nodiscard]] Result<CleanupUnitTexts> render_cleanup_units(
    const std::filesystem::path& binary_path);

// Does NOT reload or enable: installing is a systemctl decision for the CLI.
[[nodiscard]] Result<std::filesystem::path> write_cleanup_units(
    const Paths& paths, const std::filesystem::path& binary_path);

}
