#pragma once

// Logical index 0 is the oldest item still held; once full, push_back
// overwrites the oldest and dropped() counts the loss. NOT THREAD-SAFE — the
// panel's single mutex around the view state owns synchronization.
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
    // Zero capacity would silently drop every line: a caller bug, not a
    // runtime condition to absorb.
    if (capacity == 0) {
      std::fputs("tui: invariant violated: RingBuffer capacity is zero\n",
                 stderr);
      std::abort();
    }
  }

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

  [[nodiscard]] std::vector<T> snapshot() const {
    std::vector<T> out;
    out.reserve(size_);
    for (std::size_t i = 0; i < size_; ++i) {
      out.push_back(buf_[(head_ + i) % buf_.size()]);
    }
    return out;
  }

  // LOGICAL index, 0 = the oldest line still held; copy-free so the render
  // thread can draw the tail window without cloning the buffer.
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

}
