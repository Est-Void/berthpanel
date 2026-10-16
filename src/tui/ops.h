#pragma once

// One flag per operation: a panel-wide flag cannot tell "cancel the deploy I
// asked for" from "cancel the refresh that happens to be running". This mutex
// protects only this list, so nothing here ever waits on the panel lock
// (panel.h): no lock ordering to get wrong.
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tui {

struct Op {
  std::atomic<bool> cancel{false};
  std::string label;  // e.g. "deploy berthpanel-memos", shown on shutdown
};

class OpRegistry {
 public:
  // The caller owns the returned shared_ptr: a detached worker outlives its
  // registry entry at shutdown.
  std::shared_ptr<Op> begin(std::string label);

  // False when it was already gone: not an error, somebody got there first.
  bool end(const std::shared_ptr<Op>& op);

  // Flips every live flag; this is what q, Ctrl-C and SIGTERM call.
  std::size_t cancel_all();

  [[nodiscard]] std::size_t size();
  [[nodiscard]] bool empty();

  // Polls in short slices, and bounded on purpose: a worker wedged in a
  // syscall must not hold the exit hostage.
  bool wait_until_empty(std::chrono::milliseconds budget);

 private:
  std::mutex mutex_;
  std::vector<std::shared_ptr<Op>> ops_;
};

// Same discipline as wait_until_empty: a wait without a ceiling is how a UI
// hangs (§11).
[[nodiscard]] bool wait_for_flag(const std::atomic<bool>& flag,
                                 std::chrono::milliseconds budget);

}
