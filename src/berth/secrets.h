#pragma once

// src/berth/secrets.h — the per-service 0600 secrets file and its
// systemd escaping (dev.md §5). The unit loads it via EnvironmentFile=,
// so the escaping rules are systemd's EnvironmentFile rules, not a shell's:
// values are double-quoted, '\' and '"' and '$' and '`' are escaped
// inside quotes, and a newline cannot be represented at all — that is a
// loud error, never a silent truncation. Keys are validated
// ([A-Za-z_][A-Za-z0-9_]*) so a crafted key cannot inject directives.
// The file is written atomically with 0600 in a 0700 directory (§6.9).

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../paths.h"
#include "../util/result.h"

namespace berth {

using SecretEntry = std::pair<std::string, std::string>;

// Renders EnvironmentFile content (pure, tested).
[[nodiscard]] Result<std::string> render_environment_file(
    const std::vector<SecretEntry>& entries);

// Writes <secrets_dir>/<service_name>.env atomically, 0600. Duplicate keys
// are a loud error — last-one-wins would hide a caller bug.
[[nodiscard]] Result<std::filesystem::path> write_secrets(
    const Paths& paths, const std::string& service_name,
    const std::vector<SecretEntry>& entries);

}  // namespace berth
