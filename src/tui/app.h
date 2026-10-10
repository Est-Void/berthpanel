#pragma once

// src/tui/app.h — the interactive panel's entry point.
//
// ONE rule governs this module and every screen under it: the render thread
// never blocks (dev.md §5, §11). Not on `systemctl`, not on `stat`, not on a
// directory walk. Every engine call goes through run_on_worker() (panel.h),
// which hands the work to a detached std::thread and comes back through
// ScreenInteractive::Post — the panel's message queue to the render loop. A
// worker that finishes after the panel has quit checks shutting_down and
// posts nothing; the state is a shared_ptr, so it outlives this stack frame
// and no worker ever touches freed memory.
//
// The chrome (header, tab strip, footer) and the shared state live in
// panel.h; the two screens live in screens.h.

#include <memory>

#include "../core/paths.h"

namespace tui {

// Runs the panel until the user quits. Returns a process exit code (0 on a
// clean quit). Never throws across the boundary.
int run_tui(const berth::Paths& paths);

}  // namespace tui