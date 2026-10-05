#pragma once

// src/berth/sandbox.h — the mandatory build isolation (dev.md §5, §6.4).
//
// THE SANDBOX IS A BLAST-RADIUS REDUCER, NOT A TRUST BOUNDARY. It runs the
// build as the same user, under systemd-run --user; it does not make the
// code inside trustworthy. Keep that sentence with this code, in the
// threat model and in the README (dev.md §6).
//
// There is NO flag to disable or weaken it, and no environment variable
// can turn it off: sandbox_argv_body() always emits the full property set
// and there is no parameter through which a caller could drop one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../util/result.h"

namespace berth {

struct SandboxSpec {
  std::vector<std::string> argv;
  std::filesystem::path working_dir;
  std::vector<std::string> env_extra;  // "KEY=VALUE"
  // Optional TRANSIENT unit stem for the run (build_unit_name()). A named
  // unit survives its client: if berthpanel dies mid-build, the unit (and
  // its cgroup) keeps running, and reconciliation stops it BY NAME later.
  // Unnamed runs are untraceable after a crash — that is the orphan.
  std::string unit_name;
  std::size_t memory_max_mb = 2048;
  std::uint32_t tasks_max = 512;
  std::chrono::milliseconds timeout{1800000};
  std::atomic<bool>* cancel = nullptr;
};

// Everything after the binary path, deterministic and pure. One argv
// element per directive and per value: a working directory with a space
// stays one element (dev.md §11).
[[nodiscard]] std::vector<std::string> sandbox_argv_body(const SandboxSpec& spec);

// Runs the command through systemd-run --user and returns the child's exit
// code. Transport failures are Err; the build's own non-zero exit is a
// value, not an error.
[[nodiscard]] Result<int> run_in_sandbox(const SandboxSpec& spec);

struct SandboxAuditItem {
  std::string check;
  bool ok = false;
  bool required = true;  // optional items (e.g. strace) do not gate the grade
  std::string detail;
};

struct SandboxAudit {
  std::vector<SandboxAuditItem> items;
  bool strace_available = false;
  std::string network_syscalls;  // observed by strace, empty when unavailable
  [[nodiscard]] bool full_grade() const;  // every REQUIRED item ok
};

[[nodiscard]] Result<SandboxAudit> verify_sandbox();
[[nodiscard]] std::string render_sandbox_audit(const SandboxAudit& audit);

}  // namespace berth