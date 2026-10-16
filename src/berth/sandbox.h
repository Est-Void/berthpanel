#pragma once

// A BLAST-RADIUS REDUCER, NOT A TRUST BOUNDARY: the build runs as the same
// user. No flag, env var or parameter can weaken it; sandbox_argv_body() always
// emits the full property set.

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
  // A NAMED unit survives its client, so reconciliation can stop it by name.
  std::string unit_name;
  std::size_t memory_max_mb = 2048;
  std::uint32_t tasks_max = 512;
  std::chrono::milliseconds timeout{1800000};
  std::atomic<bool>* cancel = nullptr;
};

// One argv element per value: a working directory with a space stays one.
[[nodiscard]] std::vector<std::string> sandbox_argv_body(
    const SandboxSpec& spec);

// Transport failures are Err; the build's own non-zero exit is a value.
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

}
