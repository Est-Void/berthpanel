#include "cancel.h"

#include <csignal>

#include <cerrno>
#include <string>
#include <utility>

#include "core/names.h"
#include "units.h"
#include "util/errno.h"
#include "util/sleep.h"
#include "util/strutil.h"

namespace berth {

namespace {

// Handler state only. The atomic must be lock-free (checked at install) and the
// counter a volatile sig_atomic_t: the only two types sanctioned here.
std::atomic<bool>* g_exit_cancel = nullptr;
volatile std::sig_atomic_t g_signals_seen = 0;

extern "C" void berth_exit_signal_handler(int sig) {
  g_signals_seen = g_signals_seen + 1;
  if (g_exit_cancel != nullptr) {
    g_exit_cancel->store(true, std::memory_order_relaxed);
  }
  if (g_signals_seen >= 2) {
    // The user insists: the NEXT signal kills instead of being swallowed.
    ::signal(sig, SIG_DFL);
  }
}

bool cancelled(const std::atomic<bool>* cancel) {
  return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

Error cancelled_error(const std::string& unit) {
  return Error("cancel: cancelled while stopping " + unit);
}

// is-active exits nonzero for BOTH "inactive" and "activating (auto-restart)",
// so the code cannot tell a stopped unit from one that still holds its
// resources. Only a genuinely stopped state is Ok(false).
Result<bool> is_active(const std::string& unit, const StopPolicy& policy,
                       std::atomic<bool>* cancel) {
  Result<RunOutput> out =
      systemctl_user_output({"is-active", unit}, policy.stop_timeout, cancel);
  if (out.is_err())
    return std::move(out).error();
  if (out.value().exit_code == 0)
    return true;
  const std::string_view state = trim(out.value().out);
  if (state == "activating" || state == "reloading" || state == "deactivating")
    return true;
  return false;
}

// Ok(false) means "still alive", which is what makes the caller escalate.
Result<bool> wait_inactive(const std::string& unit, const StopPolicy& policy,
                           std::chrono::milliseconds budget,
                           std::atomic<bool>* cancel) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (true) {
    if (cancelled(cancel))
      return cancelled_error(unit);
    Result<bool> active = is_active(unit, policy, cancel);
    if (active.is_err())
      return std::move(active).error();
    if (!active.value())
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    interruptible_sleep(policy.poll_interval, cancel);
  }
}

// A stop timeout is tolerated: the is-active poll, not systemctl's exit, is
// the verdict. Cancellation aborts at once: Ctrl-C must not add more round
// trips.
Result<void> request_stop(const std::string& unit, const StopPolicy& policy,
                          std::atomic<bool>* cancel) {
  static_cast<void>(
      systemctl_user_code({"stop", unit}, policy.stop_timeout, cancel));
  if (cancelled(cancel))
    return cancelled_error(unit);
  return {};
}

Result<bool> escalate_to_sigkill(const std::string& unit,
                                 const StopPolicy& policy,
                                 std::atomic<bool>* cancel) {
  static_cast<void>(systemctl_user_code({"kill", "--signal=SIGKILL", unit},
                                        policy.stop_timeout, cancel));
  if (cancelled(cancel))
    return cancelled_error(unit);
  return wait_inactive(unit, policy, policy.kill_grace, cancel);
}

}
Result<void> install_exit_signalling(std::atomic<bool>* cancel) {
  if (cancel == nullptr) {
    return Error("cancel: install_exit_signalling: null flag");
  }
  if (!cancel->is_lock_free()) {
    return Error(
        "cancel: the exit flag must be lock-free to be "
        "async-signal-safe");
  }
  g_exit_cancel = cancel;
  g_signals_seen = 0;
  if (::signal(SIGINT, berth_exit_signal_handler) == SIG_ERR) {
    return Error("cancel: signal(SIGINT): " + errno_text(errno));
  }
  if (::signal(SIGTERM, berth_exit_signal_handler) == SIG_ERR) {
    return Error("cancel: signal(SIGTERM): " + errno_text(errno));
  }
  return {};
}

void uninstall_exit_signalling() {
  ::signal(SIGINT, SIG_DFL);
  ::signal(SIGTERM, SIG_DFL);
  g_exit_cancel = nullptr;
  g_signals_seen = 0;
}

SystemdUnitTerminator::SystemdUnitTerminator(StopPolicy policy)
    : policy_(policy) {}

Result<bool> SystemdUnitTerminator::stop(const std::string& unit_stem,
                                         std::atomic<bool>* cancel) {
  if (!validate_unit_name(unit_stem)) {
    return Error("cancel: invalid unit name: '" + unit_stem + "'");
  }
  const std::string unit = unit_stem + ".service";

  Result<bool> running = is_active(unit, policy_, cancel);
  if (running.is_err())
    return std::move(running).error();
  if (!running.value())
    return false;

  Result<void> stopped = request_stop(unit, policy_, cancel);
  if (stopped.is_err())
    return std::move(stopped).error();

  Result<bool> gone = wait_inactive(unit, policy_, policy_.grace, cancel);
  if (gone.is_err())
    return gone;
  if (gone.value())
    return true;

  Result<bool> killed = escalate_to_sigkill(unit, policy_, cancel);
  if (killed.is_err())
    return std::move(killed).error();
  if (killed.value())
    return true;

  return Error("cancel: " + unit + " survived SIGTERM and SIGKILL within " +
               std::to_string((policy_.grace + policy_.kill_grace).count()) +
               "ms; leaving it for the next cleanup pass");
}

}
