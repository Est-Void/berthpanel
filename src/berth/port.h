#pragma once

// src/berth/port.h — loopback port allocation (§5). Binds 127.0.0.1:0,
// reads the assigned port, closes. The kernel frees the binding on close,
// so a race window remains by design and is stated honestly: this only
// avoids obviously-colliding picks; the service's own bind is the
// authority. `avoid` lists ports already assigned to registered services.

#include <cstdint>
#include <vector>

#include "../util/result.h"

namespace berth {

[[nodiscard]] Result<std::uint16_t> pick_free_port(
    const std::vector<std::uint16_t>& avoid = {});

}  // namespace berth
