#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace berth {

// ProtectHome=read-only leaves only work_dir writable, so every toolchain's
// cache and state must live inside it; these are pure, unquoted "KEY=VALUE".
[[nodiscard]] std::vector<std::string> build_environment(
    const std::filesystem::path& work_dir);

}
