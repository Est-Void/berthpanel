#pragma once

// src/tui/ring.h — the bounded log buffer (dev.md §5: "Log line buffers are
// bounded ring buffers, not vectors drained from the front").
//
// The reason is memory, and the failure mode is specific: a long deploy
// streams thousands of lines, and a `std::vector` that gets erased from the
// front reallocates on every push and grows without a ceiling. Here the
// buffer holds at most `capacity` items forever; the oldest is overwritten
// and counted in dropped(), so the screen can say so honestly instead of
// pretending the tail is the whole story.
//
// NOT THREAD-SAFE BY ITSELF. The caller owns the synchronization — the panel
// keeps one mutex around the whole view state, so a render and a worker
// cannot interleave. This header owns no locks because a lock per buffer
// would hide the fact that reading a snapshot is a copy.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace tui {

template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(const std::size_t capacity) : buf_(capacity) {
    // A zero capacity would silently drop every line; that is a bug in the
    // caller, not a runtime condition to absorb.
    if (capacity == 0) {
      std::fputs("tui: invariant violated: RingBuffer capacity is zero\n",
                 stderr);
      std::abort();
    }
  }

  // Overwrites the oldest item once full. Never grows.
  void push_back(T value) {
    const std::size_t next = (head_ + size_) % buf_.size();
    if (size_ == buf_.size()) {
      buf_[head_] = std::move(value);
      head_ = (head_ + 1) % buf_.size();
      ++dropped_;
      return;
    }
    buf_[next] = std::move(value);
    ++size_;
  }

  // Oldest first, which is the order a log is read in.
  [[nodiscard]] std::vector<T> snapshot() const {
    std::vector<T> out;
    out.reserve(size_);
    for (std::size_t i = 0; i < size_; ++i) {
      out.push_back(buf_[(head_ + i) % buf_.size()]);
    }
    return out;
  }

  // Element at LOGICAL index, 0 = the oldest line still held. O(1) and
  // copy-free, which is what lets the render thread draw the tail window
  // without cloning the buffer every frame.
  const T& at(const std::size_t index) const {
    if (index >= size_) {
      std::fputs("tui: invariant violated: RingBuffer index out of range\n",
                 stderr);
      std::abort();
    }
    return buf_[(head_ + index) % buf_.size()];
  }

  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] std::size_t capacity() const { return buf_.size(); }
  [[nodiscard]] std::size_t dropped() const { return dropped_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }

  void clear() {
    head_ = 0;
    size_ = 0;
    dropped_ = 0;
  }

 private:
  std::vector<T> buf_;
  std::size_t head_ = 0;  // index of the oldest item
  std::size_t size_ = 0;
  std::size_t dropped_ = 0;
};

}  // namespace tui