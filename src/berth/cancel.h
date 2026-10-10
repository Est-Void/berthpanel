#pragma once

// src/berth/cancel.h — exit signalling and orphan-unit termination
// (dev.md §5). Two owners:
//
//   * install_exit_signalling(): SIGINT/SIGTERM become the cooperative cancel
//     flag every long operation already polls (§5: every wait is
//     interruptible). A SECOND signal restores the default disposition, so a
//     stuck shutdown can still be killed the old way.
//   * SystemdUnitTerminator: stopping a unit is a bounded retry loop and
//     every step in it has a deadline and a cancellation check.
//
// Orphan builds survive their parent because `systemd-run --wait` keeps the
// transient unit alive when the client dies. That is why builds run under a
// DETERMINISTIC unit name (core/names.h build_unit_name): a crashed deploy's
// unit can be found and stopped by name later — pid-based reaping after a
// crash is unsafe because pids get reused.

#include <atomic>
#include <chrono>
#include <string>

#include "../util/result.h"

namespace berth {

// Installs SIGINT/SIGTERM handlers that set *cancel. Only a lock-free
// atomic store and a volatile sig_atomic_t bump happen inside the handler
// (async-signal-safe). Err on a null flag, a non-lock-free flag, or a
// failed ::signal call — a deploy without exit signalling must not start.
[[nodiscard]] Result<void> install_exit_signalling(std::atomic<bool>* cancel);

// Back to SIG_DFL and no flag. Exists so tests (and a long-lived TUI)
// do not leak handlers into unrelated scopes.
void uninstall_exit_signalling();

// Bounds for every systemctl round trip a stop performs. Defaults are
// generous; tests shrink them.
struct StopPolicy {
  std::chrono::milliseconds stop_timeout{20000};  // per systemctl call
  std::chrono::milliseconds grace{5000};          // SIGTERM → verdict
  std::chrono::milliseconds kill_grace{5000};     // SIGKILL → verdict
  std::chrono::milliseconds poll_interval{250};
};

class UnitTerminator {
 public:
  virtual ~UnitTerminator() = default;
  // `unit_stem` is WITHOUT the .service suffix and must pass
  // validate_unit_name.
  //   Ok(true)  — the unit ran and is confirmed inactive now;
  //   Ok(false) — the unit was not running; nothing was done;
  //   Err       — cancelled, systemctl is broken, or the unit survived
  //               SIGTERM and SIGKILL: the CALLER must assume the unit
  //               may still be alive and keep the journal entry.
  [[nodiscard]] virtual Result<bool> stop(const std::string& unit_stem,
                                          std::atomic<bool>* cancel) = 0;
};

// The real terminator: `systemctl --user stop`, a bounded is-active verdict,
// then an explicit `kill --signal=SIGKILL` if the unit is still alive after
// the grace budget. Every round trip and every sleep checks the cancel flag.
class SystemdUnitTerminator final : public UnitTerminator {
 public:
  SystemdUnitTerminator() = default;
  explicit SystemdUnitTerminator(StopPolicy policy);
  [[nodiscard]] Result<bool> stop(const std::string& unit_stem,
                                  std::atomic<bool>* cancel) override;

 private:
  StopPolicy policy_;
};

}  // namespace berth
