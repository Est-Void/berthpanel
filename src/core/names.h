#pragma once

// src/core/names.h — the ONE place that knows how berthpanel names things.
// The unit prefix is load-bearing in seven places (AGENTS.md): it is checked
// here, forced onto user recipes here, and every unit path and systemd
// argument is built from it. Changing it means changing it here.

#include <cstddef>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

inline constexpr std::string_view kAppName = "berthpanel";
inline constexpr std::string_view kUnitPrefix = "berthpanel-";
inline constexpr std::string_view kBuildUnitInfix = "berthpanel-build-";
inline constexpr std::string_view kCleanupUnitStem = "berthpanel-cleanup";

inline constexpr std::size_t kMaxUnitNameLen = 128;

// The ONE unit-name validator (dev.md §11): every module that turns a name
// into a unit file, a secrets file or a work dir goes through this, so a
// creator and a remover can never disagree. Requires the prefix.
[[nodiscard]] bool validate_unit_name(std::string_view name);

// Prepends kUnitPrefix unless it is already there, so a user-supplied "myapp"
// lands as "berthpanel-myapp". Never re-validates: callers must run
// validate_unit_name on the result.
[[nodiscard]] std::string force_unit_prefix(std::string_view name);

// build_unit_name("berthpanel-x") -> "berthpanel-build-berthpanel-x". The
// build unit is transient (systemd-run --collect) and namespaced separately
// from the service so cleanup can never confuse the two.
[[nodiscard]] Result<std::string> build_unit_name(
    std::string_view service_name);

}  // namespace berth
