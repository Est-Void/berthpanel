#pragma once

// src/berth/deploy.h — the deploy orchestrator: analyse → preflight →
// fetch → build → unit → register, wrapped in the deploy lock and the
// interrupted-deploy journal (dev.md §5, §6.7).
//
// The steps live behind DeployOps so the ORDER and the failure semantics
// are testable without a network or systemd. Three rules this file exists
// to enforce:
//   * a failed registry write is NOT a successful deploy (§11 — the most
//     consequential bug of the previous implementation);
//   * on failure the journal entry STAYS, because "entry present AND lock
//     free" is precisely what __cleanup reconciles;
//   * a new deploy never starts on top of interrupted state: journal
//     entries found under the lock are swept first, loudly.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../core/paths.h"
#include "../core/state.h"
#include "../util/result.h"
#include "journal.h"
#include "recipes.h"

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

// Splits recipe.start_cmd, substitutes its placeholders, and returns the argv
// tail while writing the absolute exec path to `binary`. A non-absolute first
// token (a bare `bun`, or the `env` Gitea uses) is resolved through PATH and is
// a loud Err when absent: preflight checks the panel's PATH, but systemd's PATH
// does not include a user's ~/.bun/bin, so an unresolved name starts a unit
// that dies a second later (§11). {self}/{bin} are already absolute.
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
  // `work_dir` is the checkout: prefetch steps read the project's manifests,
  // so they MUST run there and not in whatever directory the caller lives.
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
      std::uint16_t port, const Paths& paths) = 0;
  virtual Result<void> start_service(const std::string& service_name) = 0;
  // Confirms the unit actually reached 'active'. `systemctl enable --now`
  // returns 0 even for a unit that dies a moment later, so success is only
  // claimed after this returns Ok (§11).
  virtual Result<void> await_running(const std::string& service_name,
                                     std::atomic<bool>* cancel) = 0;
  virtual Result<void> register_service(const ServiceRecord& record,
                                        const Paths& paths) = 0;
  // Stops one unit by stem (WITHOUT .service) on behalf of reconciliation.
  // Ok(false) = the unit was not running. This is the only systemd-side
  // hook reconciliation gets — deploy.cpp stays testable without systemd.
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
      std::uint16_t port, const Paths& paths) override;
  Result<void> start_service(const std::string& service_name) override;
  Result<void> await_running(const std::string& service_name,
                             std::atomic<bool>* cancel) override;
  Result<void> register_service(const ServiceRecord& record,
                                const Paths& paths) override;
  Result<bool> stop_unit(const std::string& unit_stem,
                         std::atomic<bool>* cancel) override;
};

// Polls `systemctl --user is-active` until the unit is up. Bounded, and
// honours the cancellation flag: claiming a deploy succeeded while the
// unit crash-loops is exactly the §11 failure this prevents.
[[nodiscard]] Result<std::string> wait_for_active_unit(
    const std::string& service_name, std::chrono::milliseconds budget,
    std::atomic<bool>* cancel);

// How long a unit must STAY active before a deploy may call it a success, and
// the window size the settle loop uses. Short by design: this is not a
// health check, it is the gap between "the process was spawned" and "the
// process is still there", which is exactly where a wrong start_cmd dies.
inline constexpr std::chrono::milliseconds kSettleWindow{3000};

// The settle verdict, as a PURE function so it can be tested offline: the
// states `systemctl is-active` answered AFTER the unit first reported active,
// in order. Ok() only when every one of them is "active".
//
// Why this exists: `Type=simple` means systemd calls a unit "active" the moment
// it has SPAWNED the process. A service whose start_cmd is wrong therefore
// passes an is-active check and dies a second later, looping on
// Restart=on-failure forever. The deploy reported success on a unit that was
// crash-looping — the exact §11 failure ("`enable --now` returns 0 even for a
// unit that dies a moment later"), reached through a different door.
[[nodiscard]] Result<std::string> settle_active(
    const std::string& service_name, const std::vector<std::string>& states);

// `forced_port` of 0 means "ask the recipe, then pick a free port".
[[nodiscard]] Result<DeployOutcome> run_deploy(const Paths& paths,
                                               const Recipe& recipe,
                                               std::uint16_t forced_port,
                                               DeployOps& ops,
                                               const DeployOptions& options);

// --- journal reconciliation (§6.7: entry present AND lock free ⇒
// interrupted) -----------------------------------------------------------

struct ReconcileReport {
  struct Swept {
    std::string service_name;
    std::string stage;  // where the interrupted deploy died
  };
  std::vector<Swept> swept;
  // Entries that could NOT be reconciled: <service_name, why>. Their
  // journal entries stay so a later sweep retries. One stuck entry must
  // not block sweeping the others.
  std::vector<std::pair<std::string, std::string>> failed;
  // Belt-and-suspenders (§6.7): the entry's service IS in the registry,
  // so the deploy completed and only the journal write-back was lost
  // (crash between register and journal_remove). The stale entry is
  // dropped loudly; the deployed service is NOT touched — cleanup works
  // on the journal alone and the registry owns that service now.
  std::vector<std::string> skipped_registry;
  // Work dirs that resolve OUTSIDE paths.services_dir (symlinks count):
  // the entry is never swept, not even retried into damage. Independent
  // of the registry guard — this one holds even when the registry was
  // swapped by a hostile or mistaken environment.
  std::vector<std::string> skipped_outside;
  // A live deploy owns the journal (we could not take the lock); nothing
  // was read as interrupted and nothing was touched.
  bool lock_busy = false;
};

// Sweeps interrupted deploys listed in `entries`: stops the service unit
// and the orphaned build unit, removes the work dir and the unit file,
// removes the journal entry LAST (a half-swept entry must survive for
// retry), logs each action.
//
// PRECONDITION: the caller holds the deploy lock, or has otherwise proven
// no deploy is live — "entry present" is only evidence of an interrupted
// deploy while the lock is ours (§6.7).
[[nodiscard]] Result<ReconcileReport> sweep_interrupted_entries(
    const Paths& paths, DeployOps& ops,
    const std::vector<JournalEntry>& entries, std::atomic<bool>* cancel);

// The standalone entry point for `__cleanup` (stage 4.18) and the TUI:
// loads the journal, takes the deploy lock (busy ⇒ lock_busy report, no
// error — the timer runs periodically), then sweeps.
[[nodiscard]] Result<ReconcileReport> reconcile_interrupted(
    const Paths& paths, DeployOps& ops, std::atomic<bool>* cancel);

// Human-readable sweep report for the CLI (and later the TUI).
[[nodiscard]] std::string render_reconcile_report(
    const ReconcileReport& report);

}  // namespace berth