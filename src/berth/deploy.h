#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/paths.h"
#include "../core/state.h"
#include "../util/result.h"
#include "journal.h"
#include "recipes.h"
#include "stacks.h"

namespace berth {

struct DeployOptions {
  const std::string* token = nullptr;
  std::atomic<bool>* cancel = nullptr;
};

struct DeployOutcome {
  std::string service_name;
  std::uint16_t port = 0;
  std::filesystem::path work_dir;
  std::filesystem::path unit_path;
};

struct StackDeployOutcome {
  std::string stack_name;
  std::vector<DeployOutcome> members;
};

// A non-absolute first token is resolved through PATH and is a loud Err when
// absent: systemd's PATH omits e.g. ~/.bun/bin.
[[nodiscard]] Result<std::vector<std::string>> resolve_start_argv(
    const Recipe& recipe, const std::filesystem::path& work_dir,
    std::uint16_t port, std::string& binary);

class DeployOps {
 public:
  virtual ~DeployOps() = default;
  virtual Result<void> fetch_source(const Recipe& recipe,
                                    const std::filesystem::path& work_dir,
                                    const Paths& paths,
                                    const DeployOptions& options) = 0;
  // Prefetch reads the project's manifests: it MUST run in the checkout.
  virtual Result<void> prefetch(const Recipe& recipe,
                                const std::filesystem::path& work_dir,
                                const Paths& paths,
                                const DeployOptions& options) = 0;
  virtual Result<void> build(const Recipe& recipe,
                             const std::filesystem::path& work_dir,
                             std::uint16_t port, const Paths& paths,
                             const DeployOptions& options) = 0;
  virtual Result<std::filesystem::path> write_unit_file(
      const Recipe& recipe, const std::filesystem::path& work_dir,
      std::uint16_t port, const Paths& paths,
      const std::vector<std::string>& after_units,
      const std::vector<std::string>& wants_units) = 0;
  virtual Result<void> start_service(const std::string& service_name) = 0;
  // `enable --now` returns 0 for a unit that dies a moment later, so success is
  // only claimed here; on failure the unit's own output goes to deploy.log.
  virtual Result<void> await_running(const std::string& service_name,
                                     const Paths& paths,
                                     std::atomic<bool>* cancel) = 0;
  // is-active says nothing about WHERE the app binds. A probe that cannot run
  // is a WARNING in deploy.log, never a failed deploy.
  virtual Result<void> verify_listening(const std::string& service_name,
                                        std::uint16_t port, const Paths& paths,
                                        std::atomic<bool>* cancel) = 0;
  virtual Result<void> register_service(const ServiceRecord& record,
                                        const Paths& paths) = 0;
  virtual Result<bool> stop_unit(const std::string& unit_stem,
                                 std::atomic<bool>* cancel) = 0;
};

class RealDeployOps final : public DeployOps {
 public:
  Result<void> fetch_source(const Recipe& recipe,
                            const std::filesystem::path& work_dir,
                            const Paths& paths,
                            const DeployOptions& options) override;
  Result<void> prefetch(const Recipe& recipe,
                        const std::filesystem::path& work_dir,
                        const Paths& paths,
                        const DeployOptions& options) override;
  Result<void> build(const Recipe& recipe,
                     const std::filesystem::path& work_dir, std::uint16_t port,
                     const Paths& paths, const DeployOptions& options) override;
  Result<std::filesystem::path> write_unit_file(
      const Recipe& recipe, const std::filesystem::path& work_dir,
      std::uint16_t port, const Paths& paths,
      const std::vector<std::string>& after_units,
      const std::vector<std::string>& wants_units) override;
  Result<void> start_service(const std::string& service_name) override;
  Result<void> await_running(const std::string& service_name, const Paths& paths,
                             std::atomic<bool>* cancel) override;
  Result<void> verify_listening(const std::string& service_name,
                                std::uint16_t port, const Paths& paths,
                                std::atomic<bool>* cancel) override;
  Result<void> register_service(const ServiceRecord& record,
                                const Paths& paths) override;
  Result<bool> stop_unit(const std::string& unit_stem,
                         std::atomic<bool>* cancel) override;
};

[[nodiscard]] Result<std::string> wait_for_active_unit(
    const std::string& service_name, std::chrono::milliseconds budget,
    std::atomic<bool>* cancel);

// The gap between "spawned" and "still there" — not a health check — is where a
// wrong start_cmd dies.
inline constexpr std::chrono::milliseconds kSettleWindow{3000};

// The app may bind a moment after systemd says "active".
inline constexpr std::chrono::milliseconds kListenWindow{3000};

struct PortReport;

// Nothing on the port and wildcard-only (0.0.0.0/::) are both Err; only a
// loopback listener on `expected_port` is Ok.
[[nodiscard]] Result<void> verify_loopback_listener(
    const PortReport& report, std::uint16_t expected_port,
    const std::string& service_name);

// The is-active states AFTER the unit first reported active. Ok() only when all
// are "active": `Type=simple` calls a unit active at spawn, so a wrong
// start_cmd passes the first check and crash-loops forever.
[[nodiscard]] Result<std::string> settle_active(
    const std::string& service_name, const std::vector<std::string>& states,
    const std::string& evidence = {});

[[nodiscard]] std::string parse_systemctl_show(std::string_view show);

[[nodiscard]] std::string bound_journal_tail(std::string_view text,
                                             std::size_t max_lines,
                                             std::size_t max_bytes);

// `forced_port` of 0 means "ask the recipe, then pick a free port".
[[nodiscard]] Result<DeployOutcome> run_deploy(const Paths& paths,
                                               const Recipe& recipe,
                                               std::uint16_t forced_port,
                                               DeployOps& ops,
                                               const DeployOptions& options);

// A stack is one operation: every member's journal entry is written before the
// first member runs, so an interruption at any point leaves the whole group
// reconciled by cleanup. Members run in after/wants order.
[[nodiscard]] Result<StackDeployOutcome> run_stack_deploy(
    const Paths& paths, const Stack& stack, DeployOps& ops,
    const DeployOptions& options);

struct ReconcileReport {
  struct Swept {
    std::string service_name;
    std::string stage;
    std::string stack;  // group stem when the entry is a stack member
  };
  std::vector<Swept> swept;
  // One stuck entry must not block sweeping the others; theirs stay for retry.
  std::vector<std::pair<std::string, std::string>> failed;
  // In the registry ⇒ only the journal write-back was lost. Drop the entry.
  std::vector<std::string> skipped_registry;
  // Work dirs resolving OUTSIDE services_dir: never swept, not even retried.
  std::vector<std::string> skipped_outside;
  bool lock_busy = false;
};

// Removes the journal entry LAST — a half-swept entry must survive for retry.
// PRECONDITION: the caller holds the deploy lock, else "entry present" proves
// nothing.
[[nodiscard]] Result<ReconcileReport> sweep_interrupted_entries(
    const Paths& paths, DeployOps& ops,
    const std::vector<JournalEntry>& entries, std::atomic<bool>* cancel);

// Lock busy ⇒ lock_busy report, no error: the cleanup timer runs periodically.
[[nodiscard]] Result<ReconcileReport> reconcile_interrupted(
    const Paths& paths, DeployOps& ops, std::atomic<bool>* cancel);

[[nodiscard]] std::string render_reconcile_report(
    const ReconcileReport& report);

}
