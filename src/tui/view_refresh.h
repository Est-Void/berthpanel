#pragma once

#include <chrono>

namespace tui {

inline constexpr std::chrono::milliseconds kAutoRefreshInterval{2500};

struct AutoRefreshState {
  bool on_owning_tab = false;
  bool in_flight = false;
  bool shutting_down = false;
  std::chrono::milliseconds since_last{0};
};

// Every false case is a worker the panel must not spawn: a tick overlapping an
// in-flight refresh is the duplicate-worker bug, a tick after q has no owner.
[[nodiscard]] bool auto_refresh_due(const AutoRefreshState& state);

}
