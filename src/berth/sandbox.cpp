#include "sandbox.h"

#include <cstdio>

#include <string>

#include "../core/atomic.h"
#include "../core/names.h"
#include "../util/fs.h"
#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/tmpdir.h"
#include "../util/which.h"
#include "units.h"

namespace berth {

namespace {

constexpr std::chrono::milliseconds kProbeTimeout{30000};

Result<int> probe(const std::vector<std::string>& body) {
  Result<std::string> binary = resolve_executable("systemd-run");
  if (binary.is_err())
    return std::move(binary).error();
  std::vector<std::string> argv{std::move(binary).value()};
  argv.insert(argv.end(), body.begin(), body.end());
  RunLimits limits;
  limits.timeout = kProbeTimeout;
  Result<RunOutput> out = run(argv, limits);
  if (out.is_err())
    return std::move(out).error();
  return out.value().exit_code;
}

std::vector<std::string> probe_argv(std::vector<std::string> properties,
                                    const std::filesystem::path& work,
                                    const std::string& command, bool shell_c) {
  std::vector<std::string> argv = {"--user",  "--wait", "--pipe", "--quiet",
                                   "--collect"};
  for (const std::string& property : properties)
    argv.push_back("--property=" + property);
  argv.push_back("--working-directory=" + work.string());
  argv.push_back("--property=ReadWritePaths=" + work.string());
  argv.push_back("--");
  argv.push_back("/bin/sh");
  if (shell_c)
    argv.push_back("-c");
  argv.push_back(command);
  return argv;
}

Result<std::filesystem::path> write_probe(const std::filesystem::path& work,
                                          const std::string& name,
                                          const std::string& body) {
  const std::filesystem::path script = work / name;
  Result<void> written = atomic_write(script, body, kPermPrivateDir);
  if (written.is_err())
    return std::move(written).error();
  return script;
}

SandboxAuditItem probe_item(std::string check, const Result<int>& out,
                            int expected, std::string prefix = "exit ",
                            bool required = true) {
  return {.check = std::move(check),
          .ok = out.is_ok() && out.value() == expected,
          .required = required,
          .detail = out.is_ok() ? prefix + std::to_string(out.value())
                                : out.error().ctx()};
}

// strace is optional: a host without it reports a skipped check, not a failure.
void audit_syscalls(SandboxAudit& audit, const std::filesystem::path& work) {
  Result<std::string> strace = resolve_executable("strace");
  if (strace.is_err()) {
    audit.items.push_back({.check = "syscall audit (strace)",
                           .ok = true,
                           .required = false,
                           .detail = "skipped: strace is not installed on this host"});
    return;
  }
  audit.strace_available = true;
  const std::filesystem::path trace = work / "trace.log";
  SandboxSpec spec{
      .argv = {std::move(strace).value(), "-f", "-e", "trace=network", "-o",
               trace.string(), "/bin/sh", "-c", "echo probe"},
      .working_dir = work,
      .env_extra = {},
      .unit_name = {},
      .memory_max_mb = 256,
      .tasks_max = 512,
  };
  const Result<int> traced = run_in_sandbox(spec);
  Result<std::string> observed = read_file(trace);
  std::string network_calls;
  if (observed.is_ok()) {
    for (const std::string& line : split(observed.value(), "\n")) {
      if (line.find("socket(") != std::string::npos ||
          line.find("connect(") != std::string::npos) {
        network_calls += line + "\n";
      }
    }
  }
  audit.network_syscalls = network_calls;
  audit.items.push_back({.check = "syscall audit (strace)",
                         .ok = traced.is_ok() && network_calls.empty(),
                         .required = false,
                         .detail = traced.is_err() ? traced.error().ctx()
                                                  : "no socket/connect seen"});
}

}
std::vector<std::string> sandbox_argv_body(const SandboxSpec& spec) {
  std::vector<std::string> argv = {
      "--user",
      "--wait",
      "--pipe",
      "--quiet",
      "--collect",
      "--property=MemoryMax=" + std::to_string(spec.memory_max_mb) + "M",
      "--property=TasksMax=" + std::to_string(spec.tasks_max),
  };
  // A build must not be less contained than the service it builds.
  for (const std::string_view directive : hardening_directives()) {
    argv.push_back("--property=" + std::string(directive));
  }
  if (!spec.working_dir.empty()) {
    const Result<std::string> raw = raw_systemd_path(spec.working_dir);
    if (raw.is_err()) {
      // Invariant violation: run_in_sandbox validates first.
      std::fputs("berth: fatal: sandbox working_dir is invalid: ", stderr);
      std::fputs(spec.working_dir.c_str(), stderr);
      std::fputc('\n', stderr);
      std::abort();
    }
    argv.push_back("--working-directory=" + raw.value());
    // ReadWritePaths= is a LIST: a space would split it in two.
    argv.push_back("--property=ReadWritePaths=" +
                   escape_path_list_value(raw.value()));
  }
  for (const std::string& entry : spec.env_extra) {
    argv.push_back("--setenv=" + entry);
  }
  if (!spec.unit_name.empty()) {
    if (!validate_unit_name(spec.unit_name)) {
      // Invariant violation: an unnamed build orphans the moment we die.
      std::fputs("berth: fatal: sandbox unit_name is invalid: ", stderr);
      std::fputs(spec.unit_name.c_str(), stderr);
      std::fputc('\n', stderr);
      std::abort();
    }
    argv.push_back("--unit=" + spec.unit_name);
  }
  argv.push_back("--");
  argv.insert(argv.end(), spec.argv.begin(), spec.argv.end());
  return argv;
}

Result<int> run_in_sandbox(const SandboxSpec& spec) {
  if (!spec.working_dir.empty()) {
    Result<std::string> raw = raw_systemd_path(spec.working_dir);
    if (raw.is_err())
      return std::move(raw).error();
  }
  Result<std::string> binary = resolve_executable("systemd-run");
  if (binary.is_err()) {
    return Error("sandbox: systemd-run is required and was not found: " +
                 binary.error().ctx());
  }
  std::vector<std::string> argv{std::move(binary).value()};
  const std::vector<std::string> body = sandbox_argv_body(spec);
  argv.insert(argv.end(), body.begin(), body.end());
  RunLimits limits;
  limits.timeout = spec.timeout;
  limits.cancel = spec.cancel;
  Result<RunOutput> out = run(argv, limits);
  if (out.is_err())
    return std::move(out).error();
  // --pipe carries the step's own stderr; a bare exit code has no why.
  if (out.value().exit_code != 0) {
    std::string detail(trim(out.value().err));
    if (detail.empty())
      detail = trim(out.value().out);
    if (!detail.empty()) {
      return Error("sandbox: unit " +
                   (spec.unit_name.empty() ? "(anonymous)" : spec.unit_name) +
                   " failed with exit " +
                   std::to_string(out.value().exit_code) + ": " + detail);
    }
  }
  return out.value().exit_code;
}

bool SandboxAudit::full_grade() const {
  if (items.empty())
    return false;
  for (const SandboxAuditItem& item : items) {
    if (item.required && !item.ok)
      return false;
  }
  return true;
}

Result<SandboxAudit> verify_sandbox() {
  Result<std::string> binary = resolve_executable("systemd-run");
  if (binary.is_err())
    return std::move(binary).error();

  const Result<TempDir> scratch = TempDir::create("verify");
  if (scratch.is_err())
    return std::move(scratch).error();
  const std::filesystem::path work = scratch.value().path();

  SandboxAudit audit;

  const Result<int> exit_code =
      probe({"--user", "--wait", "--pipe", "--quiet", "--collect", "--",
             "/bin/sh", "-c", "exit 7"});
  audit.items.push_back(probe_item("exit code passthrough", exit_code, 7, "got "));

  const Result<std::filesystem::path> memprobe =
      write_probe(work, "memprobe.sh",
                  "#!/bin/sh\n"
                  "cg=$(awk -F: '/^0::/ {print $3}' /proc/self/cgroup)\n"
                  "max=$(cat \"/sys/fs/cgroup${cg}/memory.max\" 2>/dev/null)\n"
                  "test \"$max\" = 67108864\n");
  if (memprobe.is_err())
    return std::move(memprobe).error();
  const Result<int> memory = probe(probe_argv(
      {"MemoryMax=64M"}, work, memprobe.value().string(), false));
  audit.items.push_back(probe_item("MemoryMax applied", memory, 0));

  const Result<std::filesystem::path> privprobe =
      write_probe(work, "privprobe.sh",
                  "#!/bin/sh\n"
                  "awk '/^NoNewPrivs:/ { found = ($2 == 1) } "
                  "END { exit(found ? 0 : 1) }' /proc/self/status\n");
  if (privprobe.is_err())
    return std::move(privprobe).error();
  const Result<int> no_privs = probe(probe_argv(
      {"NoNewPrivileges=yes"}, work, privprobe.value().string(), false));
  audit.items.push_back(probe_item("NoNewPrivileges applied", no_privs, 0));

  const Result<int> write_outside =
      probe(probe_argv({"ProtectSystem=strict"}, work,
                       "! touch /etc/berthpanel-sandbox-probe 2>/dev/null", true));
  audit.items.push_back(
      probe_item("writes outside the work dir are refused", write_outside, 0));

  const Result<int> write_inside = probe(
      probe_argv({"ProtectSystem=strict"}, work, "touch sandbox-ok && test -f sandbox-ok", true));
  audit.items.push_back(
      probe_item("the work dir stays writable", write_inside, 0));

  audit_syscalls(audit, work);
  return audit;
}

std::string render_sandbox_audit(const SandboxAudit& audit) {
  std::string out;
  for (const SandboxAuditItem& item : audit.items) {
    if (!item.required && item.detail.rfind("skipped", 0) == 0) {
      out += "  skip ";
    } else {
      out += item.ok ? "  ok   " : "  FAIL ";
    }
    out += item.check;
    if (!item.detail.empty()) {
      out += "  (";
      out += item.detail;
      out += ")";
    }
    out += "\n";
  }
  out += audit.full_grade() ? "\n  grade FULL\n"
                            : "\n  grade INCOMPLETE — a deploy must not run\n";
  return out;
}

}
