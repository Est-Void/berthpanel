#pragma once

// Prefetch DOWNLOADS, never EXECUTES repo code (§6.5): a positive allowlist.

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

// work_dir is the checkout: every download-only command is a PROJECT command.
[[nodiscard]] Result<std::size_t> run_prefetch(
    const std::vector<std::vector<std::string>>& steps,
    const std::filesystem::path& work_dir, std::atomic<bool>* cancel,
    std::chrono::milliseconds per_step_timeout =
        std::chrono::milliseconds(600000));

[[nodiscard]] std::string render_prefetch_report(
    const std::vector<std::vector<std::string>>& steps,
    const std::vector<std::string>& failures);

}
