#pragma once

// NEUTRAL module on purpose: the deploy engine must never call into tui/ (§5).

#include <string>
#include <string_view>

#include "core/applog.h"  // log_timestamp
#include "core/paths.h"
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<void> deploylog_append(const Paths& paths,
                                            std::string_view line);

}
