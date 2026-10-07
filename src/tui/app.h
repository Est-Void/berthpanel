#pragma once

// src/tui/app.h — the interactive panel's entry point.
//
// ONE rule governs this module and every screen under it: the render thread
// never blocks (dev.md §5, §11). Not on `systemctl`, not on `stat`, not on a
// directory walk. Every engine call goes through spawn(), which hands the
// work to a detached std::thread and comes back through
// ScreenInteractive::Post — the panel's message queue to the render loop. A
// worker that finishes after the panel has quit sets a flag in the shared
// state and posts nothing; the shared state is a shared_ptr, so it outlives
// this stack frame and no worker ever touches freed memory.

#include <memory>

#include "../core/paths.h"

namespace tui {

// Runs the panel until the user quits. Returns a process exit code (0 on a
// clean quit). Never throws across the boundary.
int run_tui(const berth::Paths& paths);

}  // namespace tui