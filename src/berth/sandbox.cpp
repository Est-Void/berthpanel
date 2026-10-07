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

}  // namespace

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
  // The same hardening set the produced unit gets, from the same table: a
  // build must not be less contained than the service it builds.
  for (const std::string_view directive : hardening_directives()) {
    argv.push_back("--property=" + std::string(directive));
  }
  if (!spec.working_dir.empty()) {
    const Result<std::string> raw = raw_systemd_path(spec.working_dir);
    if (raw.is_err()) {
      // Invariant violation: a caller-supplied spec with a bad working dir
      // must not silently lose its ReadWritePaths. run_in_sandbox validates
      // first and returns a clean error.
      std::fputs("berth: fatal: sandbox working_dir is invalid: ", stderr);
      std::fputs(spec.working_dir.c_str(), stderr);
      std::fputc('\n', stderr);
      std::abort();
    }
    argv.push_back("--working-directory=" + raw.value());
    argv.push_back("--property=ReadWritePaths=" + raw.value());
  }
  for (const std::string& entry : spec.env_extra) {
    argv.push_back("--setenv=" + entry);
  }
  if (!spec.unit_name.empty()) {
    if (!validate_unit_name(spec.unit_name)) {
      // Invariant violation: unit_name comes from build_unit_name(),
      // which validates. A silently unnamed build would be an orphan the
      // moment berthpanel dies, so this must be loud (§11).
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
  // --pipe routes the unit's streams back to us: a failed step must carry
  // its own stderr, or a build failure is an exit code with no why (§11).
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

  const std::vector<std::string> base = {"--user", "--wait", "--pipe",
                                         "--quiet", "--collect"};

  auto with_probe = [&](const std::vector<std::string>& properties,
                        const std::string& shell) {
    std::vector<std::string> argv = base;
    for (const std::string& property : properties) {
      argv.push_back("--property=" + property);
    }
    argv.push_back("--working-directory=" + work.string());
    argv.push_back("--property=ReadWritePaths=" + work.string());
    argv.push_back("--");
    argv.push_back("/bin/sh");
    argv.push_back("-c");
    argv.push_back(shell);
    return argv;
  };

  const Result<int> exit_code =
      probe({"--user", "--wait", "--pipe", "--quiet", "--collect", "--",
             "/bin/sh", "-c", "exit 7"});
  audit.items.push_back(
      {.check = "exit code passthrough",
       .ok = exit_code.is_ok() && exit_code.value() == 7,
       .detail = exit_code.is_ok() ? "got " + std::to_string(exit_code.value())
                                   : exit_code.error().ctx()});

  const std::filesystem::path memprobe = work / "memprobe.sh";
  const std::string memprobe_body =
      "#!/bin/sh\n"
      "cg=$(awk -F: '/^0::/ {print $3}' /proc/self/cgroup)\n"
      "max=$(cat \"/sys/fs/cgroup${cg}/memory.max\" 2>/dev/null)\n"
      "test \"$max\" = 67108864\n";
  Result<void> probe_written =
      atomic_write(memprobe, memprobe_body, kPermPrivateDir);
  if (probe_written.is_err())
    return std::move(probe_written).error();
  std::vector<std::string> mem_argv = base;
  mem_argv.push_back("--property=MemoryMax=64M");
  mem_argv.push_back("--working-directory=" + work.string());
  mem_argv.push_back("--property=ReadWritePaths=" + work.string());
  mem_argv.push_back("--");
  mem_argv.push_back("/bin/sh");
  mem_argv.push_back(std::string(memprobe.string()));
  const Result<int> memory = probe(mem_argv);
  audit.items.push_back(
      {.check = "MemoryMax applied",
       .ok = memory.is_ok() && memory.value() == 0,
       .detail = memory.is_ok() ? "exit " + std::to_string(memory.value())
                                : memory.error().ctx()});

  const std::filesystem::path privprobe = work / "privprobe.sh";
  const std::string privprobe_body =
      "#!/bin/sh\n"
      "awk '/^NoNewPrivs:/ { found = ($2 == 1) } END { exit(found ? 0 : 1) }' "
      "/proc/self/status\n";
  Result<void> priv_written =
      atomic_write(privprobe, privprobe_body, kPermPrivateDir);
  if (priv_written.is_err())
    return std::move(priv_written).error();
  std::vector<std::string> priv_argv = base;
  priv_argv.push_back("--property=NoNewPrivileges=yes");
  priv_argv.push_back("--working-directory=" + work.string());
  priv_argv.push_back("--property=ReadWritePaths=" + work.string());
  priv_argv.push_back("--");
  priv_argv.push_back("/bin/sh");
  priv_argv.push_back(std::string(privprobe.string()));
  const Result<int> no_privs = probe(priv_argv);
  audit.items.push_back(
      {.check = "NoNewPrivileges applied",
       .ok = no_privs.is_ok() && no_privs.value() == 0,
       .detail = no_privs.is_ok() ? "exit " + std::to_string(no_privs.value())
                                  : no_privs.error().ctx()});

  const Result<int> write_outside =
      probe(with_probe({"ProtectSystem=strict"},
                       "! touch /etc/berthpanel-sandbox-probe 2>/dev/null"));
  audit.items.push_back(
      {.check = "writes outside the work dir are refused",
       .ok = write_outside.is_ok() && write_outside.value() == 0,
       .detail = write_outside.is_ok()
                     ? "exit " + std::to_string(write_outside.value())
                     : write_outside.error().ctx()});

  const Result<int> write_inside = probe(with_probe(
      {"ProtectSystem=strict"}, "touch sandbox-ok && test -f sandbox-ok"));
  audit.items.push_back(
      {.check = "the work dir stays writable",
       .ok = write_inside.is_ok() && write_inside.value() == 0,
       .detail = write_inside.is_ok()
                     ? "exit " + std::to_string(write_inside.value())
                     : write_inside.error().ctx()});

  Result<std::string> strace = resolve_executable("strace");
  if (strace.is_err()) {
    audit.items.push_back(
        {.check = "syscall audit (strace)",
         .ok = true,
         .required = false,
         .detail = "skipped: strace is not installed on this host"});
  } else {
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
                           .detail = traced.is_err()
                                         ? traced.error().ctx()
                                         : "no socket/connect seen"});
  }

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

}  // namespace berth