#include "units.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "../core/atomic.h"
#include "../core/names.h"
#include "../util/errno.h"
#include "../util/fs.h"
#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/which.h"

namespace berth {

namespace {

// Blast radius only, never a trust boundary. sandbox.cpp renders this SAME set;
// CTAD means a dropped entry can never leave an empty slot behind.
constexpr auto kHardening = std::to_array<std::string_view>({
    "NoNewPrivileges=yes",
    "PrivateTmp=yes",
    "PrivateDevices=yes",
    "ProtectSystem=strict",
    "ProtectHome=read-only",
    "ProtectKernelTunables=yes",
    "ProtectKernelModules=yes",
    "ProtectControlGroups=yes",
    "RestrictSUIDSGID=yes",
    "RestrictRealtime=yes",
    "RestrictNamespaces=yes",
    "LockPersonality=yes",
    "CapabilityBoundingSet=",
    "AmbientCapabilities=",
    "SystemCallArchitectures=native",
    "SystemCallFilter=@system-service",
    "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6",
});

constexpr std::filesystem::perms kUnitPerms =
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
    std::filesystem::perms::group_read | std::filesystem::perms::others_read;

constexpr std::string_view kUnitComment =
    "# Managed by berthpanel — regenerated on every deploy; do not edit.\n"
    "# Hardening limits blast radius; it is not a security boundary.\n";

[[nodiscard]] Result<std::string> no_control_chars(const std::string_view value,
                                                   const std::string& what) {
  for (const char c : value) {
    if (c == '\n' || c == '\r' || c == '\0') {
      return Error("units: " + what +
                   " contains a newline or NUL, which a "
                   "unit file cannot represent");
    }
  }
  return std::string(value);
}

}
std::span<const std::string_view> hardening_directives() {
  return kHardening;
}

Result<std::string> quote_exec_word(const std::string_view arg) {
  Result<std::string> checked = no_control_chars(arg, "exec argument");
  if (checked.is_err())
    return std::move(checked).error();
  std::string out = "\"";
  for (const char c : arg) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '%':
        out += "%%";  // systemd specifier escape
        break;
      default:
        out += c;
    }
  }
  out += "\"";
  return out;
}

Result<std::string> quote_unit_value(const std::string_view value) {
  Result<std::string> checked = no_control_chars(value, "directive value");
  if (checked.is_err())
    return std::move(checked).error();
  std::string out = "\"";
  for (const char c : value) {
    // '%' is doubled, never backslashed: the backslash would join the value.
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
      continue;
    }
    if (c == '%') {
      out += "%%";
      continue;
    }
    out += c;
  }
  out += "\"";
  return out;
}

Result<std::string> raw_systemd_path(const std::filesystem::path& path) {
  const std::string value = path.string();
  if (value.empty())
    return Error("units: refusing an empty path");
  if (value.front() != '/') {
    return Error("units: path directives need an absolute path: " + value);
  }
  for (const char c : value) {
    if (c == '\n' || c == '\r' || c == '\0') {
      return Error("units: path contains a control character");
    }
  }
  return value;
}

std::string escape_path_list_value(const std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '\\' || c == ' ' || c == '\t') {
      out += '\\';
      out += c;
      continue;
    }
    if (c == '%') {
      out += "%%";
      continue;
    }
    out += c;
  }
  return out;
}

Result<std::string> render_exec_start(const std::string_view binary,
                                      const std::vector<std::string>& args) {
  Result<std::string> head = quote_exec_word(binary);
  if (head.is_err())
    return std::move(head).error();
  std::string line = std::move(head).value();
  for (const std::string& arg : args) {
    Result<std::string> word = quote_exec_word(arg);
    if (word.is_err())
      return std::move(word).error();
    line += " ";
    line += std::move(word).value();
  }
  return line;
}

Result<std::string> substitute_placeholders(
    const std::string_view tmpl,
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::string out;
  out.reserve(tmpl.size());
  std::size_t pos = 0;
  while (pos < tmpl.size()) {
    const std::size_t open = tmpl.find('{', pos);
    if (open == std::string_view::npos) {
      out += tmpl.substr(pos);
      break;
    }
    const std::size_t close = tmpl.find('}', open);
    if (close == std::string_view::npos) {
      return Error("units: unbalanced '{' in template: " + std::string(tmpl));
    }
    out += tmpl.substr(pos, open - pos);
    const std::string key(tmpl.substr(open + 1, close - open - 1));
    const auto it =
        std::find_if(values.begin(), values.end(),
                     [&](const auto& kv) { return kv.first == key; });
    if (it == values.end()) {
      return Error("units: unknown placeholder {" + key + "}");
    }
    out += it->second;
    pos = close + 1;
  }
  return out;
}

namespace {

bool valid_order_unit(const std::string& unit) {
  // A stem must not already carry the suffix, or the unit would be
  // `<stem>.service.service`. validate_unit_name rejects the characters a
  // member id could use to inject a directive.
  return validate_unit_name(unit) && !unit.ends_with(".service");
}

Result<void> check_unit_spec(const UnitSpec& spec) {
  if (!validate_unit_name(spec.service_name)) {
    return Error("units: invalid service name: '" + spec.service_name + "'");
  }
  // §8: without them a unit is a runaway that grinds the whole user session.
  if (spec.limits.empty()) {
    return Error("units: refusing to render " + spec.service_name +
                 " without memory_max and tasks_max");
  }
  if (spec.exec_binary.empty()) {
    return Error("units: exec binary is empty for " + spec.service_name);
  }
  return {};
}

Result<void> append_unit_section(std::string* out, const UnitSpec& spec) {
  // display_name is free user text: a newline there would inject directives.
  Result<std::string> description =
      no_control_chars(spec.description, "description");
  if (description.is_err())
    return std::move(description).error();
  *out += "[Unit]\n";
  *out +=
      "Description=" + description.value() + " (" + spec.service_name + ")\n";
  *out += "After=network-online.target\n";
  *out += "Wants=network-online.target\n";
  for (const std::string& unit : spec.after_units) {
    if (!valid_order_unit(unit))
      return Error("units: invalid After unit: '" + unit + "'");
    *out += "After=";
    *out += unit;
    *out += ".service\n";
  }
  for (const std::string& unit : spec.wants_units) {
    if (!valid_order_unit(unit))
      return Error("units: invalid Wants unit: '" + unit + "'");
    *out += "Wants=";
    *out += unit;
    *out += ".service\n";
  }
  *out += "\n";
  return {};
}

void append_policy_section(std::string* out, const UnitSpec& spec) {
  *out += "Restart=on-failure\n";
  *out += "RestartSec=2\n";
  *out += "TimeoutStopSec=20\n";
  *out += "UMask=0077\n";
  *out += "StandardOutput=journal\n";
  *out += "StandardError=journal\n";
  *out += "\n";
  *out += "# Resource limits are mandatory (dev.md §8).\n";
  *out += "MemoryMax=" + spec.limits.memory_max + "\n";
  *out += "TasksMax=" + std::to_string(spec.limits.tasks_max) + "\n";
  *out += "\n";
}

void append_hardening_section(std::string* out, const UnitSpec& spec) {
  *out += "# Hardening: blast radius only, never a trust boundary.\n";
  for (const std::string_view directive : hardening_directives()) {
    *out += directive;
    *out += "\n";
  }
  // One directive per path (a space-joined list becomes nonexistent paths), and
  // ReadWritePaths= is a list, so the path is escaped as one.
  for (const std::filesystem::path& path : spec.read_write_paths) {
    *out += "ReadWritePaths=" + escape_path_list_value(path.string()) + "\n";
  }
  *out += "\n";
}

Result<void> append_path_directives(std::string* out, const UnitSpec& spec) {
  if (!spec.working_dir.empty()) {
    Result<std::string> wd = raw_systemd_path(spec.working_dir);
    if (wd.is_err())
      return std::move(wd).error();
    *out += "WorkingDirectory=" + std::move(wd).value() + "\n";
  }
  if (!spec.secrets_file.empty()) {
    Result<std::string> env_file = raw_systemd_path(spec.secrets_file);
    if (env_file.is_err())
      return std::move(env_file).error();
    *out += "EnvironmentFile=" + std::move(env_file).value() + "\n";
  }
  for (const std::string& kv : spec.extra_environment) {
    Result<std::string> entry = quote_unit_value(kv);
    if (entry.is_err())
      return std::move(entry).error();
    *out += "Environment=" + std::move(entry).value() + "\n";
  }
  return {};
}

Result<void> append_exec_start(std::string* out, const UnitSpec& spec) {
  Result<std::string> exec =
      render_exec_start(spec.exec_binary, spec.exec_args);
  if (exec.is_err())
    return std::move(exec).error();
  *out += "ExecStart=" + std::move(exec).value() + "\n";
  return {};
}

}
Result<std::string> render_unit(const UnitSpec& spec) {
  const Result<void> checked = check_unit_spec(spec);
  if (checked.is_err())
    return std::move(checked).error();

  std::string out = std::string(kUnitComment);
  const Result<void> unit_section = append_unit_section(&out, spec);
  if (unit_section.is_err())
    return std::move(unit_section).error();

  out += "[Service]\n";
  out += "Type=simple\n";
  const Result<void> paths = append_path_directives(&out, spec);
  if (paths.is_err())
    return std::move(paths).error();
  const Result<void> exec = append_exec_start(&out, spec);
  if (exec.is_err())
    return std::move(exec).error();
  append_policy_section(&out, spec);
  append_hardening_section(&out, spec);

  out += "[Install]\n";
  out += "WantedBy=default.target\n";
  return out;
}

Result<std::filesystem::path> write_unit(const Paths& paths,
                                         const UnitSpec& spec) {
  Result<std::string> text = render_unit(spec);
  if (text.is_err())
    return std::move(text).error();
  Result<void> dir = ensure_dir(paths.units_dir);
  if (dir.is_err())
    return std::move(dir).error();
  const std::filesystem::path target =
      paths.units_dir / (spec.service_name + ".service");
  Result<void> written = atomic_write(target, text.value(), kUnitPerms);
  if (written.is_err())
    return std::move(written).error();
  return target;
}

namespace {

Result<RunOutput> systemctl_user_run(const std::vector<std::string>& args,
                                     const std::chrono::milliseconds timeout,
                                     std::atomic<bool>* cancel) {
  Result<std::string> binary = resolve_executable("systemctl");
  if (binary.is_err())
    return std::move(binary).error();
  std::vector<std::string> argv{std::move(binary).value(), "--user"};
  argv.insert(argv.end(), args.begin(), args.end());
  RunLimits limits;
  limits.timeout = timeout;
  limits.cancel = cancel;
  return run(argv, limits);
}

}
Result<int> systemctl_user_code(const std::vector<std::string>& args,
                                const std::chrono::milliseconds timeout,
                                std::atomic<bool>* cancel) {
  Result<RunOutput> out = systemctl_user_output(args, timeout, cancel);
  if (out.is_err())
    return std::move(out).error();
  return out.value().exit_code;
}

Result<RunOutput> systemctl_user_output(const std::vector<std::string>& args,
                                        const std::chrono::milliseconds timeout,
                                        std::atomic<bool>* cancel) {
  return systemctl_user_run(args, timeout, cancel);
}

Result<void> systemctl_user(const std::vector<std::string>& args) {
  Result<RunOutput> out =
      systemctl_user_run(args, std::chrono::milliseconds(20000), nullptr);
  if (out.is_err())
    return std::move(out).error();
  if (out.value().exit_code != 0) {
    return Error("units: systemctl --user " + (args.empty() ? "" : args[0]) +
                 " failed with exit " + std::to_string(out.value().exit_code) +
                 ": " + out.value().err);
  }
  return {};
}

Result<void> systemctl_daemon_reload() {
  return systemctl_user({"daemon-reload"});
}

Result<void> systemctl_enable_now(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("units: invalid service name: '" + service_name + "'");
  }
  Result<void> reloaded = systemctl_daemon_reload();
  if (reloaded.is_err())
    return reloaded;
  return systemctl_user({"enable", "--now", service_name + ".service"});
}

Result<void> systemctl_disable(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("units: invalid service name: '" + service_name + "'");
  }
  Result<void> stopped =
      systemctl_user({"disable", "--now", service_name + ".service"});
  if (stopped.is_err())
    return stopped;
  return systemctl_daemon_reload();
}

Result<CleanupUnitTexts> render_cleanup_units(
    const std::filesystem::path& binary_path) {
  if (binary_path.empty() || !binary_path.is_absolute()) {
    return Error("units: cleanup service needs an absolute binary path");
  }
  // Quoted PER ARGUMENT: a $HOME with a space stays one ExecStart word.
  if (!validate_unit_name(kCleanupUnitStem)) {
    // An invalid name here would be an unremovable orphan unit.
    std::fputs("berth: fatal: cleanup unit stem is invalid\n", stderr);
    std::abort();
  }
  Result<std::string> exec = quote_exec_word(binary_path.string());
  if (exec.is_err())
    return std::move(exec).error();

  CleanupUnitTexts texts;
  texts.service = std::string(kUnitComment);
  texts.service += "[Unit]\n";
  texts.service += "Description=berthpanel cleanup of interrupted deploys\n\n";
  texts.service += "[Service]\n";
  texts.service += "Type=oneshot\n";
  // --quiet: the timer fires hourly and must not spam the journal.
  texts.service += "ExecStart=" + exec.value() + " __cleanup --quiet\n";

  texts.timer = std::string(kUnitComment);
  texts.timer += "[Unit]\n";
  texts.timer +=
      "Description=Periodic cleanup of interrupted berthpanel deploys\n\n";
  texts.timer += "[Timer]\n";
  texts.timer += "OnCalendar=hourly\n";
  texts.timer += "Persistent=true\n\n";
  texts.timer += "[Install]\n";
  texts.timer += "WantedBy=timers.target\n";
  return texts;
}

Result<std::filesystem::path> write_cleanup_units(
    const Paths& paths, const std::filesystem::path& binary_path) {
  Result<CleanupUnitTexts> texts = render_cleanup_units(binary_path);
  if (texts.is_err())
    return std::move(texts).error();
  Result<void> dir = ensure_dir(paths.units_dir);
  if (dir.is_err())
    return std::move(dir).error();
  const std::filesystem::path service =
      paths.units_dir / (std::string(kCleanupUnitStem) + ".service");
  Result<void> wrote_service =
      atomic_write(service, texts.value().service, kUnitPerms);
  if (wrote_service.is_err())
    return std::move(wrote_service).error();
  const std::filesystem::path timer =
      paths.units_dir / (std::string(kCleanupUnitStem) + ".timer");
  Result<void> wrote_timer =
      atomic_write(timer, texts.value().timer, kUnitPerms);
  if (wrote_timer.is_err())
    return std::move(wrote_timer).error();
  return timer;
}

}
