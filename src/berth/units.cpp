// src/berth/units.cpp — see units.h for the contract.

#include "units.h"

#include <algorithm>
#include <string>
#include <utility>

#include "../atomic.h"
#include "../state.h"
#include "../util/errno.h"
#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/which.h"

namespace berth {

namespace {

// Hardening chosen to bound a runaway service: memory, tasks, filesystem
// writes and syscall surface. None of it is a guarantee — the same user
// runs the process either way (dev.md §6, "blast-radius reducer").
constexpr std::string_view kUnitComment =
    "# Managed by berthpanel — regenerated on every deploy; do not edit.\n"
    "# Hardening limits blast radius; it is not a security boundary.\n";

[[nodiscard]] Result<std::string> no_control_chars(const std::string_view value,
                                                   const std::string& what) {
  for (const char c : value) {
    if (c == '\n' || c == '\r' || c == '\0') {
      return Error("units: " + what + " contains a newline or NUL, which a "
                                        "unit file cannot represent");
    }
  }
  return std::string(value);
}

}  // namespace

Result<std::string> quote_exec_word(const std::string_view arg) {
  Result<std::string> checked = no_control_chars(arg, "exec argument");
  if (checked.is_err()) return std::move(checked).error();
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
  if (checked.is_err()) return std::move(checked).error();
  std::string out = "\"";
  for (const char c : value) {
    // systemd unit syntax: '"' and '\\' take a backslash escape, '%' is
    // doubled (a backslash there would become part of the value).
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

Result<std::string> render_exec_start(const std::string_view binary,
                                      const std::vector<std::string>& args) {
  Result<std::string> head = quote_exec_word(binary);
  if (head.is_err()) return std::move(head).error();
  std::string line = std::move(head).value();
  for (const std::string& arg : args) {
    Result<std::string> word = quote_exec_word(arg);
    if (word.is_err()) return std::move(word).error();
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
    const auto it = std::find_if(values.begin(), values.end(),
                                 [&](const auto& kv) { return kv.first == key; });
    if (it == values.end()) {
      return Error("units: unknown placeholder {" + key + "}");
    }
    out += it->second;
    pos = close + 1;
  }
  return out;
}

Result<std::string> render_unit(const UnitSpec& spec) {
  if (!validate_unit_name(spec.service_name)) {
    return Error("units: invalid service name: '" + spec.service_name + "'");
  }
  if (spec.limits.empty()) {
    // §8: memory_max and tasks_max are mandatory. A unit without them is
    // a runaway waiting to grind the whole user session.
    return Error("units: refusing to render " + spec.service_name +
                 " without memory_max and tasks_max");
  }
  if (spec.exec_binary.empty()) {
    return Error("units: exec binary is empty for " + spec.service_name);
  }

  std::string out = std::string(kUnitComment);
  out += "[Unit]\n";
  out += "Description=" + spec.description + " (" + spec.service_name + ")\n";
  out += "After=network-online.target\n";
  out += "Wants=network-online.target\n\n";

  out += "[Service]\n";
  out += "Type=simple\n";

  if (!spec.working_dir.empty()) {
    Result<std::string> wd = quote_unit_value(spec.working_dir.string());
    if (wd.is_err()) return std::move(wd).error();
    out += "WorkingDirectory=" + std::move(wd).value() + "\n";
  }
  if (!spec.secrets_file.empty()) {
    Result<std::string> env_file = quote_unit_value(spec.secrets_file.string());
    if (env_file.is_err()) return std::move(env_file).error();
    out += "EnvironmentFile=" + std::move(env_file).value() + "\n";
  }
  for (const std::string& kv : spec.extra_environment) {
    Result<std::string> entry = quote_unit_value(kv);
    if (entry.is_err()) return std::move(entry).error();
    out += "Environment=" + std::move(entry).value() + "\n";
  }

  Result<std::string> exec = render_exec_start(spec.exec_binary, spec.exec_args);
  if (exec.is_err()) return std::move(exec).error();
  out += "ExecStart=" + std::move(exec).value() + "\n";

  out += "Restart=on-failure\n";
  out += "RestartSec=2\n";
  out += "TimeoutStopSec=20\n";
  out += "UMask=0077\n";
  out += "StandardOutput=journal\n";
  out += "StandardError=journal\n";
  out += "\n";

  out += "# Resource limits are mandatory (dev.md §8): a runaway service must\n";
  out += "# never be able to grind the whole user session.\n";
  out += "MemoryMax=" + spec.limits.memory_max + "\n";
  out += "TasksMax=" + std::to_string(spec.limits.tasks_max) + "\n";
  out += "\n";

  out += "# Hardening: blast radius only, never a trust boundary.\n";
  out += "NoNewPrivileges=yes\n";
  out += "PrivateTmp=yes\n";
  out += "PrivateDevices=yes\n";
  out += "ProtectSystem=strict\n";
  out += "ProtectHome=read-only\n";
  out += "ProtectKernelTunables=yes\n";
  out += "ProtectKernelModules=yes\n";
  out += "ProtectControlGroups=yes\n";
  out += "RestrictSUIDSGID=yes\n";
  out += "RestrictRealtime=yes\n";
  out += "RestrictNamespaces=yes\n";
  out += "LockPersonality=yes\n";
  out += "CapabilityBoundingSet=\n";
  out += "AmbientCapabilities=\n";
  out += "SystemCallArchitectures=native\n";
  out += "SystemCallFilter=@system-service\n";
  out += "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6\n";
  // One directive per path — a space-joined list would become two
  // nonexistent paths and the namespace would fail to set up (§11).
  for (const std::filesystem::path& path : spec.read_write_paths) {
    Result<std::string> value = quote_unit_value(path.string());
    if (value.is_err()) return std::move(value).error();
    out += "ReadWritePaths=" + std::move(value).value() + "\n";
  }
  out += "\n[Install]\n";
  out += "WantedBy=default.target\n";
  return out;
}

Result<std::filesystem::path> write_unit(const Paths& paths,
                                         const UnitSpec& spec) {
  Result<std::string> text = render_unit(spec);
  if (text.is_err()) return std::move(text).error();
  Result<void> dir = ensure_dir(paths.units_dir);
  if (dir.is_err()) return std::move(dir).error();
  const std::filesystem::path target =
      paths.units_dir / (spec.service_name + ".service");
  Result<void> written = atomic_write(target, text.value(),
                                      std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write |
                                          std::filesystem::perms::group_read |
                                          std::filesystem::perms::others_read);
  if (written.is_err()) return std::move(written).error();
  return target;
}

Result<void> systemctl_user(const std::vector<std::string>& args) {
  Result<std::string> binary = resolve_executable("systemctl");
  if (binary.is_err()) return std::move(binary).error();
  std::vector<std::string> argv{std::move(binary).value(), "--user"};
  argv.insert(argv.end(), args.begin(), args.end());
  RunLimits limits;
  limits.timeout = std::chrono::milliseconds(20000);
  Result<RunOutput> out = run(argv, limits);
  if (out.is_err()) return std::move(out).error();
  if (out.value().exit_code != 0) {
    return Error("units: systemctl --user " + (args.empty() ? "" : args[0]) +
                 " failed with exit " + std::to_string(out.value().exit_code) +
                 ": " + out.value().err);
  }
  return {};
}

Result<void> systemctl_daemon_reload() { return systemctl_user({"daemon-reload"}); }

Result<void> systemctl_enable_now(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("units: invalid service name: '" + service_name + "'");
  }
  Result<void> reloaded = systemctl_daemon_reload();
  if (reloaded.is_err()) return reloaded;
  return systemctl_user({"enable", "--now", service_name + ".service"});
}

Result<void> systemctl_disable(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("units: invalid service name: '" + service_name + "'");
  }
  Result<void> stopped = systemctl_user({"disable", "--now", service_name + ".service"});
  if (stopped.is_err()) return stopped;
  return systemctl_daemon_reload();
}

}  // namespace berth
