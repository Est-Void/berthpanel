#pragma once

// The ONE place that knows how berthpanel names things; the prefix is
// load-bearing.

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

// The ONE unit-name validator (§11) — every module that turns a name into a
// unit file, a secrets file or a work dir goes through it. Requires the prefix.
[[nodiscard]] bool validate_unit_name(std::string_view name);

// Never re-validates: callers must run validate_unit_name on the result.
[[nodiscard]] std::string force_unit_prefix(std::string_view name);

// The build unit is transient and namespaced apart from the service, so cleanup
// can never confuse the two.
[[nodiscard]] Result<std::string> build_unit_name(
    std::string_view service_name);

}
