#pragma once

// tests/test_env.h — shared test fixtures: scoped env mutation (must run
// under env_test_lock, dev.md §9) and temp dirs with a random suffix (§9:
// never a predictable /tmp path).

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "util/env.h"

// Saves the variable's old value on construction and restores it on
// destruction, so a failing test cannot poison later ones.
class ScopedEnv {
 public:
  explicit ScopedEnv(const char* name) : name_(name) {
    if (const char* old = ::getenv(name); old != nullptr) old_ = old;
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

  ~ScopedEnv() { restore(); }

  void set(const std::string& value) {
    ::setenv(name_, value.c_str(), 1);
    touched_ = true;
  }
  void unset() {
    ::unsetenv(name_);
    touched_ = true;
  }

 private:
  void restore() {
    if (!touched_) return;
    if (old_.has_value()) {
      ::setenv(name_, old_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

  const char* name_;
  std::optional<std::string> old_;
  bool touched_ = false;
};

// RAII temp directory with a random suffix; removed recursively on scope
// exit (best effort — an undeletable temp must not fail the suite).
class TempDir {
 public:
  TempDir() {
    std::string tmpl = "/tmp/berthpanel-test-XXXXXX";
    const char* made = ::mkdtemp(tmpl.data());
    if (made == nullptr) {
      std::fputs("test: mkdtemp failed\n", stderr);
      std::abort();
    }
    path_ = made;
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};
