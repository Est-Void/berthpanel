#pragma once

// SIGINT/SIGTERM become the cooperative cancel flag every long operation polls;
// a SECOND signal restores SIG_DFL. `systemd-run --wait` outlives its client,
// so a crashed deploy's build unit is reaped by name, never by pid.

#include <atomic>
#include <chrono>
#include <string>

#include "../util/result.h"

namespace berth {

// Inside the handler: only a lock-free atomic store and a volatile sig_atomic_t
// bump. Err on a null or non-lock-free flag: no signalling, no deploy.
[[nodiscard]] Result<void> install_exit_signalling(std::atomic<bool>* cancel);

// Back to SIG_DFL, so tests and a long-lived TUI do not leak handlers.
void uninstall_exit_signalling();

struct StopPolicy {
  std::chrono::milliseconds stop_timeout{20000};
  std::chrono::milliseconds grace{5000};
  std::chrono::milliseconds kill_grace{5000};
  std::chrono::milliseconds poll_interval{250};
};

class UnitTerminator {
 public:
  virtual ~UnitTerminator() = default;
  // `unit_stem` carries no .service suffix. Ok(true) = stopped and confirmed
  // inactive, Ok(false) = was not running. Err = cancelled, systemctl broken,
  // or it survived SIGTERM and SIGKILL: assume alive, KEEP the journal.
  [[nodiscard]] virtual Result<bool> stop(const std::string& unit_stem,
                                          std::atomic<bool>* cancel) = 0;
};

// stop, bounded verdict, then SIGKILL; every round trip and sleep checks it.
class SystemdUnitTerminator final : public UnitTerminator {
 public:
  SystemdUnitTerminator() = default;
  explicit SystemdUnitTerminator(StopPolicy policy);
  [[nodiscard]] Result<bool> stop(const std::string& unit_stem,
                                  std::atomic<bool>* cancel) override;

 private:
  StopPolicy policy_;
};

}
