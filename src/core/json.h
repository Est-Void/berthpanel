#pragma once

// A document that will not parse is copied aside and the error says so — never
// read as empty, or the panel would forget every service it manages (§11).

#include <filesystem>

#include <nlohmann/json.hpp>

#include "util/result.h"

namespace berth {

// On a parse error the file is quarantined and the error names both the failure
// and where the bad bytes went.
[[nodiscard]] Result<nlohmann::json> json_load(
    const std::filesystem::path& path);

// Atomic 0600 write.
[[nodiscard]] Result<void> json_store(const std::filesystem::path& path,
                                      const nlohmann::json& value);

// Copies `path` aside to a free `<name>.corrupt[.N]`; the original survives.
[[nodiscard]] Result<std::filesystem::path> quarantine_corrupt(
    const std::filesystem::path& path);

}
