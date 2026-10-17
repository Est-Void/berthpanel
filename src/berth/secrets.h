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

// Rewrites ONE key in place, preserving every other line (comments included),
// then stores the file 0600 atomically. No restart happens here.
[[nodiscard]] Result<std::filesystem::path> set_environment_value(
    const Paths& paths, const std::string& service_name, const std::string& key,
    const std::string& value);

// Keys only — never a value — so a caller can render an editor safely.
[[nodiscard]] Result<std::vector<std::string>> list_secret_keys(
    const Paths& paths, const std::string& service_name);

}
