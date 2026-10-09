#pragma once

// src/util/subprocess.h — the only way this codebase spawns a process
// (dev.md §4: system()/popen() are banned, argv arrays only). Takes an
// explicit argv and a mandatory positive timeout; captures stdout/stderr
// with a size cap; the child's stdin is /dev/null so nothing can hang
// waiting for input. A nonzero exit or a signal death is *not* a Result
// error — it is decoded into RunOutput and the caller decides (e.g. git
// exit codes are data). run() fails only on spawn errors, wait/poll errors,
// timeout or cancellation. Owns no global state; env/cwd are inherited
// (per-call env lands with gitclone in stage 4.8).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

#include "util/result.h"

namespace berth {

struct RunOutput {
  int exit_code = -1;      // WEXITSTATUS when WIFEXITED; -1 otherwise.
  bool signaled = false;   // child died on a signal.
  int term_sig = 0;        // WTERMSIG when signaled.
  bool truncated = false;  // a capture stream hit the cap; excess discarded.
  std::string out;
  std::string err;
};

struct RunLimits {
  std::chrono::milliseconds timeout{};  // mandatory; must be positive.
  std::size_t max_capture = std::size_t{1024} * 1024;
  std::atomic<bool>* cancel = nullptr;  // polled; SIGKILL + Err when set.
  // "KEY=VALUE" entries overriding or extending the inherited environment.
  // Used for GIT_ASKPASS (dev.md §6.3); never for tokens in argv.
  const std::vector<std::string>* extra_env = nullptr;
  // The child's working directory. Empty means "inherit ours", which is right
  // for most commands and WRONG for anything that reads a project tree: a
  // `go mod download` run in the panel's own directory finds no go.mod and says
  // "no modules specified", which looks like the user's repo is broken.
  const std::filesystem::path* cwd = nullptr;
};

[[nodiscard]] Result<RunOutput> run(const std::vector<std::string>& argv,
                                    const RunLimits& limits);

}  // namespace berth
