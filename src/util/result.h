#pragma once

// src/util/result.h — the one error type for recoverable failures
// (dev.md §4, §12.1). Every recoverable failure returns berth::Result<T>;
// every error carries a context chain built with with_context(); discarding
// a Result is a compile error via [[nodiscard]]. Invariant violations abort
// and never travel through this type. Exceptions never cross module
// boundaries. Owns no I/O and no global state.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace berth {

class Error {
 public:
  explicit Error(std::string message) : root_(std::move(message)) {}

  Error(const Error&) = default;
  Error(Error&&) = default;
  Error& operator=(const Error&) = default;
  Error& operator=(Error&&) = default;
  ~Error() = default;

  // Adds an outer context layer. Call at each boundary that adds
  // information; see with_context().
  void add_context(std::string context) {
    layers_.push_back(std::move(context));
  }

  // Full chain, outermost context first, root cause last:
  // "registering service: reading state.json: enoent".
  [[nodiscard]] std::string ctx() const {
    std::string out;
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
      out += *it;
      out += ": ";
    }
    out += root_;
    return out;
  }

  // The root cause without any context layers.
  [[nodiscard]] const std::string& root() const { return root_; }

 private:
  std::string root_;
  std::vector<std::string> layers_;
};

template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : data_(std::move(value)) {}
  Result(Error error) : data_(std::move(error)) {}

  [[nodiscard]] bool is_ok() const { return data_.index() == 0; }
  [[nodiscard]] bool is_err() const { return !is_ok(); }

  [[nodiscard]] const T& value() const& {
    check_ok();
    return std::get<0>(data_);
  }

  [[nodiscard]] T&& value() && {
    check_ok();
    return std::get<0>(std::move(data_));
  }

  [[nodiscard]] const Error& error() const& {
    check_err();
    return std::get<1>(data_);
  }

  [[nodiscard]] Error&& error() && {
    check_err();
    return std::get<1>(std::move(data_));
  }

 private:
  void check_ok() const {
    if (!is_ok()) {
      std::fputs("berth: invariant violated: Result::value() on error: ",
                 stderr);
      std::fputs(std::get<1>(data_).ctx().c_str(), stderr);
      std::fputc('\n', stderr);
      std::abort();
    }
  }

  void check_err() const {
    if (is_ok()) {
      std::fputs("berth: invariant violated: Result::error() on value\n",
                 stderr);
      std::abort();
    }
  }

  std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() : data_(std::monostate{}) {}
  Result(Error error) : data_(std::move(error)) {}

  [[nodiscard]] bool is_ok() const { return data_.index() == 0; }
  [[nodiscard]] bool is_err() const { return !is_ok(); }

  [[nodiscard]] const Error& error() const& {
    check_err();
    return std::get<1>(data_);
  }

  [[nodiscard]] Error&& error() && {
    check_err();
    return std::get<1>(std::move(data_));
  }

 private:
  void check_err() const {
    if (is_ok()) {
      std::fputs("berth: invariant violated: Result::error() on value\n",
                 stderr);
      std::abort();
    }
  }

  std::variant<std::monostate, Error> data_;
};

// Wraps an error with one more context layer. Use at every boundary that
// adds information; the bare error three frames deep is worthless (§4).
[[nodiscard]] inline Error with_context(Error e, std::string context) {
  e.add_context(std::move(context));
  return e;
}

template <typename T>
[[nodiscard]] Result<T> with_context(Result<T> r, std::string context) {
  if (r.is_err()) {
    Error error = std::move(r).error();
    error.add_context(std::move(context));
    return error;
  }
  return r;
}

[[nodiscard]] inline Result<void> with_context(Result<void> r,
                                               std::string context) {
  if (r.is_err()) {
    Error error = std::move(r).error();
    error.add_context(std::move(context));
    return error;
  }
  return r;
}

}  // namespace berth
