#pragma once

#include <optional>
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<std::optional<bool>> env_flag(const char* name);

}  // namespace berth
