#pragma once

// The render thread never blocks (dev.md §5): no systemctl, no stat, no
// directory walk. Everything goes through run_on_worker() and comes back via
// ScreenInteractive::Post; a worker finishing after quit posts nothing.
#include <memory>

#include "../core/paths.h"

namespace tui {

int run_tui(const berth::Paths& paths);

}
