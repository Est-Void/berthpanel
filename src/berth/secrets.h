#pragma once

// systemd's EnvironmentFile escaping, not a shell's: values are double-quoted
// with '\', '"', '$' and '`' escaped inside, and a newline is a loud error.
// Keys are validated so a crafted key cannot inject a directive.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

using SecretEntry = std::pair<std::string, std::string>;

[[nodiscard]] Result<std::string> render_environment_file(
    const std::vector<SecretEntry>& entries);

// Duplicate keys are a loud error: last-one-wins would hide a caller bug.
[[nodiscard]] Result<std::filesystem::path> write_secrets(
    const Paths& paths, const std::string& service_name,
    const std::vector<SecretEntry>& entries);

}
