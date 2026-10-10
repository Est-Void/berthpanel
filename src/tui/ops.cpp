// src/tui/ops.cpp — see ops.h for the contract.

#include "ops.h"

#include <thread>

namespace tui {

namespace {
// Long enough that an operation stopping is noticed promptly, short enough
// that a shutdown never sleeps longer than it must.
constexpr auto kPollSlice = std::chrono::milliseconds(20);
}  // namespace

std::shared_ptr<Op> OpRegistry::begin(std::string label) {
  auto op = std::make_shared<Op>();
  op->label = std::move(label);
  const std::lock_guard<std::mutex> guard(mutex_);
  ops_.push_back(op);
  return op;
}

bool OpRegistry::end(const std::shared_ptr<Op>& op) {
  const std::lock_guard<std::mutex> guard(mutex_);
  for (std::size_t i = 0; i < ops_.size(); ++i) {
    if (ops_[i] == op) {
      ops_.erase(ops_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

std::size_t OpRegistry::cancel_all() {
  const std::lock_guard<std::mutex> guard(mutex_);
  for (const std::shared_ptr<Op>& op : ops_) {
    op->cancel.store(true, std::memory_order_relaxed);
  }
  return ops_.size();
}

std::size_t OpRegistry::size() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return ops_.size();
}

bool OpRegistry::empty() {
  return size() == 0;
}

bool OpRegistry::wait_until_empty(const std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (true) {
    if (empty()) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kPollSlice);
  }
}

bool wait_for_flag(const std::atomic<bool>& flag,
                   const std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (true) {
    if (flag.load(std::memory_order_relaxed)) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kPollSlice);
  }
}

}  // namespace tui