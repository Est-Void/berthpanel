#pragma once

// src/util/tmpdir.h — a private scratch directory with a random suffix,
// removed when it goes out of scope. Every temporary path the project
// creates carries a random suffix (§9); this is the one way to make one.

#include <filesystem>
#include <string_view>
#include <utility>

#include "util/result.h"

namespace berth {

class TempDir {
 public:
  TempDir() = default;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&& other) noexcept : path_(std::exchange(other.path_, {})) {}
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      path_ = std::exchange(other.path_, {});
    }
    return *this;
  }
  ~TempDir();

  // Creates /tmp/berthpanel-<prefix>-XXXXXX with 0700.
  [[nodiscard]] static Result<TempDir> create(std::string_view prefix);

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  explicit TempDir(std::filesystem::path path) : path_(std::move(path)) {}

  std::filesystem::path path_;
};

}  // namespace berth
