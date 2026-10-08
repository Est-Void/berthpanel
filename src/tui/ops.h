#pragma once

// src/tui/ops.h — the registry of running long operations, and the one place
// cancellation is implemented (dev.md §6.7).
//
// Stage 6.7 in one sentence: q and Ctrl-C must make every long operation
// UNWIND, not be ignored. The old panel's permanent retry never consulted the
// exit flag, so a Ctrl-C during a stalled request cost seconds of frozen screen
// and then ran reconciliation with a worker still alive (§11).
//
// Two design points that follow from that:
//
//   * ONE flag per operation. A panel-wide flag cannot distinguish "cancel the
//     deploy I asked for" from "cancel the log refresh that happens to be
//     running", and a shared flag means any screen can kill any other one.
//   * The registry is SELF-CONTAINED. It owns the only mutex besides
//     panel.mutex, and it protects exactly one thing: this list. Everything
//     else in the view state stays under the panel's single mutex (panel.h), so
//     there is no lock ordering to get wrong — nothing here ever takes the
//     panel lock, and nothing under the panel lock ever waits on this one while
//     holding it.
//
// No FTXUI, no engine, no I/O: this header is testable on its own, which
// matters because "the cancel flag reaches the worker" is the invariant the
// whole quit path rests on.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tui {

// One long operation's cancellation state.
struct Op {
  std::atomic<bool> cancel{false};
  std::string label;  // "deploy berthpanel-memos" — for the shutdown report
};

class OpRegistry {
 public:
  // Registers an operation and hands back its own cancel flag. The caller owns
  // the returned shared_ptr: the operation outlives the registry entry when a
  // worker is detached at shutdown.
  std::shared_ptr<Op> begin(std::string label);

  // Deregisters a finished operation. Returns false when it was already gone;
  // that is not an error, it just means somebody got there first.
  bool end(const std::shared_ptr<Op>& op);

  // Flips EVERY live flag and returns how many were flipped. This is what q,
  // Ctrl-C and SIGTERM all call.
  std::size_t cancel_all();

  [[nodiscard]] std::size_t size();
  [[nodiscard]] bool empty();

  // Waits until every operation has deregistered, or the budget runs out.
  // Returns whether it finished in time. Polls in short slices: an operation
  // that stops is noticed within milliseconds, and a shutdown never sleeps
  // longer than it must. Bounded on purpose — a worker wedged in a syscall must
  // not hold the exit hostage.
  bool wait_until_empty(std::chrono::milliseconds budget);

 private:
  std::mutex mutex_;
  std::vector<std::shared_ptr<Op>> ops_;
};

// Waits for a flag, in short slices, up to the budget. Returns whether it
// arrived in time. The same discipline as OpRegistry::wait_until_empty and for
// the same reason: a wait without a ceiling is how a UI hangs (§11).
[[nodiscard]] bool wait_for_flag(const std::atomic<bool>& flag,
                                 std::chrono::milliseconds budget);

}  // namespace tui