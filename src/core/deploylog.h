#pragma once

// src/core/deploylog.h — the deploy log sink, deliberately in a NEUTRAL
// module: the deployment engine must never call into tui/ (dev.md §5
// layering), which is why the old engine could not run without a TUI. Same
// append-once-per-line discipline as netlog, both on top of applog. The TUI
// will tee these lines into its ring buffer; the headless path prints them.

#include <string>
#include <string_view>

#include "core/applog.h"  // log_timestamp
#include "core/paths.h"
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<void> deploylog_append(const Paths& paths,
                                            std::string_view line);

}  // namespace berth
