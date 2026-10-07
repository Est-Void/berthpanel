#pragma once

// src/core/flags.h — tri-state parsing of the BERTHPANEL_* switches. Absent
// or EMPTY means unset (§11): an exported-but-empty variable is not a "true".

#include <optional>

#include "util/result.h"

namespace berth {

// Ok(nullopt) when the variable is unset or empty; Ok(true)/Ok(false) when
// it holds a truthy/falsey token.
[[nodiscard]] Result<std::optional<bool>> env_flag(const char* name);

}  // namespace berth
