#pragma once

// src/deploylog.h — the deploy log sink, deliberately in a NEUTRAL module:
// the deployment engine must never call into tui/ (dev.md §5 layering),
// which is why the old engine could not run without a TUI. Same 0600
// append-once-per-line discipline as netlog. The TUI will tee these lines
// into its ring buffer; the headless path prints them.

#include <string>
#include <string_view>

#include "paths.h"
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<void> deploylog_append(const Paths& paths,
                                            std::string_view line);

[[nodiscard]] std::string deploylog_timestamp();

}  // namespace berth