#pragma once

// Reached over pinned anycast IPs, so the fallback cannot be defeated by
// breaking the name server it would otherwise need.

#include <atomic>
#include <chrono>
#include <string_view>
#include <vector>

#include "../../core/paths.h"
#include "resolver.h"
#include "util/result.h"

namespace berth {

// A response with no usable address at all is a loud error, not a success.
[[nodiscard]] Result<std::vector<ResolvedIp>> parse_doh_answers(
    std::string_view body);

[[nodiscard]] DohFallback doh_fallback();

}
