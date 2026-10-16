#pragma once

// The only process spawner: argv arrays only, system()/popen() banned (§4).
// A nonzero exit or a signal death is NOT an Err — it is decoded into
// RunOutput and the caller decides (git exit codes are data). Err is reserved
// for spawn/wait/poll errors, timeout and cancellation.

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
  bool signaled = false;
  int term_sig = 0;
  bool truncated = false;  // a capture stream hit the cap; excess discarded.
  std::string out;
  std::string err;
};

struct RunLimits {
  std::chrono::milliseconds timeout{};
  std::size_t max_capture = std::size_t{1024} * 1024;
  std::atomic<bool>* cancel = nullptr;  // polled; SIGKILL + Err when set.
  // "KEY=VALUE" entries over the inherited env. This is the GIT_ASKPASS route
  // for tokens (§6.3); tokens never go in argv.
  const std::vector<std::string>* extra_env = nullptr;
  // Empty means "inherit ours", which is WRONG for anything that reads a
  // project tree: a `go mod download` run here finds no go.mod and blames the
  // user's repo.
  const std::filesystem::path* cwd = nullptr;
};

[[nodiscard]] Result<RunOutput> run(const std::vector<std::string>& argv,
                                    const RunLimits& limits);

}
