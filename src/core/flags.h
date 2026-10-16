#pragma once

// Absent or EMPTY is unset (§11): an exported empty variable is not a "true".

#include <optional>

#include "util/result.h"

namespace berth {

[[nodiscard]] Result<std::optional<bool>> env_flag(const char* name);

}
