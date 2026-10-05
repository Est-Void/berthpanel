// src/berth/cancel.cpp — see cancel.h for the contract.

#include "cancel.h"

#include <csignal>
#include <ctime>

#include <cerrno>
#include <string>

#include "../util/errno.h"
#include "../util/subprocess.h"
#include "../util/which.h"
#include "state.h"
#include "units.h"

namespace berth {

namespace {

// Touched from the signal handler only: a flag pointer plus a counter.
// The atomic must be lock-free (checked at install time) so the store is
// async-signal-safe; the counter is a volatile sig_atomic_t, the type the
// standard sanctions for exactly this.
std::atomic<bool>* g_exit_cancel = nullptr;
volatile std::sig_atomic_t g_signals_seen = 0;

extern "C" void berth_exit_signal_handler(int sig) {
  g_signals_seen = g_signals_seen + 1;
  if (g_exit_cancel != nullptr) {
    g_exit_cancel->store(true, std::memory_order_relaxed);
  }
  if (g_signals_seen >= 2) {
    // The user insists. Restore the default disposition so the NEXT
    // signal kills the process the old way instead of being swallowed
    // by an unresponsive shutdown path.
    ::signal(sig, SIG_DFL);
  }
}

void sleep_interruptible(const std::chrono::milliseconds budget,
                         std::atomic<bool>* cancel) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto slice = budget < left ? budget : left;
    timespec ts{slice.count() / 1000, (slice.count() % 1000) * 1000 * 1000};
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
  }
}

}  // namespace

Result<std::string> build_unit_name(const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("cancel: invalid service name: '" + service_name + "'");
  }
  const std::string stem = "berthpanel-build-" + service_name;
  // The build name is a DIFFERENT string than the service name and gets
  // its own validation: a legal service name can still push the build
  // name past the length cap, and a build we cannot name is a build we
  // could never stop (§11: validate every value that reaches a path).
  if (!validate_unit_name(stem)) {
    return Error("cancel: service name '" + service_name +
                 "' does not leave room for a valid build unit name '" +
                 stem + "'");
  }
  return stem;
}

Result<void> install_exit_signalling(std::atomic<bool>* cancel) {
  if (cancel == nullptr) {
    return Error("cancel: install_exit_signalling: null flag");
  }
  if (!cancel->is_lock_free()) {
    return Error("cancel: the exit flag must be lock-free to be "
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
  const auto cancelled = [&]() {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
  };
  const auto cancelled_error = [&]() {
    return Error("cancel: cancelled while stopping " + unit);
  };
  const auto is_active = [&]() -> Result<bool> {
    // is-active's nonzero exit means "not active", which is DATA here,
    // not a failure — hence the _code variant.
    Result<int> code =
        systemctl_user_code({"is-active", unit}, policy_.stop_timeout, cancel);
    if (code.is_err()) return std::move(code).error();
    return code.value() == 0;
  };
  const auto wait_inactive = [&](std::chrono::milliseconds budget) -> Result<bool> {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (true) {
      if (cancelled()) return cancelled_error();
      Result<bool> active = is_active();
      if (active.is_err()) return std::move(active).error();
      if (!active.value()) return true;
      if (std::chrono::steady_clock::now() >= deadline) return false;
      sleep_interruptible(policy_.poll_interval, cancel);
    }
  };

  Result<bool> running = is_active();
  if (running.is_err()) return std::move(running).error();
  if (!running.value()) return false;

  // SIGTERM. A timeout on the stop call is tolerated on purpose: the
  // request has reached systemd either way, and the is-active poll below
  // — not systemctl's own exit — is the verdict. A cancellation aborts
  // immediately: a user-issued Ctrl-C must never degrade into more
  // systemctl round trips (§11: every wait is interruptible).
  static_cast<void>(
      systemctl_user_code({"stop", unit}, policy_.stop_timeout, cancel));
  if (cancelled()) return cancelled_error();

  Result<bool> gone = wait_inactive(policy_.grace);
  if (gone.is_err()) return std::move(gone).error();
  if (gone.value()) return true;

  // Still alive: SIGKILL the cgroup directly, then one final bounded
  // verdict. Reaching the error below keeps the journal entry, so the
  // next sweep retries with fresh budgets instead of declaring victory.
  static_cast<void>(systemctl_user_code(
      {"kill", "--signal=SIGKILL", unit}, policy_.stop_timeout, cancel));
  if (cancelled()) return cancelled_error();

  Result<bool> killed = wait_inactive(policy_.kill_grace);
  if (killed.is_err()) return std::move(killed).error();
  if (killed.value()) return true;
  return Error("cancel: " + unit + " survived SIGTERM and SIGKILL within " +
               std::to_string((policy_.grace + policy_.kill_grace).count()) +
               "ms; leaving it for the next cleanup pass");
}

}  // namespace berth
