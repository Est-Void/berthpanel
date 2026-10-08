#pragma once

// src/berth/prefetch.h — the host phase that fills tool caches so the
// sandboxed build can run offline. The invariant it exists for: prefetch
// DOWNLOADS, it never EXECUTES repository code (dev.md §6.5). A hostile
// setup.py or postinstall must not run outside the sandbox, so steps are
// checked against a POSITIVE allowlist of download-only commands and the
// package managers are required to carry their no-scripts flag. A guard
// that cannot fail is not a guard (§11): unrecognised commands are a
// loud error, not a warning.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "../util/result.h"

namespace berth {

[[nodiscard]] Result<void> validate_prefetch_step(
    const std::vector<std::string>& argv);

// `work_dir` is the checkout the steps READ: every download-only command is a
// PROJECT command (`go mod download`, `pip download -r requirements.txt`), and
// run without a working directory they run in whatever directory the caller
// happens to be in — where there is no manifest, and they fail with a message
// about the user's project that is really about ours.
[[nodiscard]] Result<std::size_t> run_prefetch(
    const std::vector<std::vector<std::string>>& steps,
    const std::filesystem::path& work_dir, std::atomic<bool>* cancel,
    std::chrono::milliseconds per_step_timeout =
        std::chrono::milliseconds(600000));

[[nodiscard]] std::string render_prefetch_report(
    const std::vector<std::vector<std::string>>& steps,
    const std::vector<std::string>& failures);

}  // namespace berth