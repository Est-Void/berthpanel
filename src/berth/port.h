#pragma once

// The kernel frees the binding on close, so a race window remains by design.
// `avoid` lists ports already assigned to registered services.

#include <cstdint>
#include <vector>

#include "../util/result.h"

namespace berth {

[[nodiscard]] Result<std::uint16_t> pick_free_port(
    const std::vector<std::uint16_t>& avoid = {});

}
