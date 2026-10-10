#pragma once

// src/tui/view_refresh.h — the auto-refresh DECISION for the two lists that
// update on their own (My Services and the Deploy catalog), as a pure
// function. The TIMER lives in the TUI (app.cpp): a background ticker wakes on
// the cadence and asks this whether a refresh is due, so the "wrong tab /
// in flight / shutting down ⇒ no" rule is testable without a terminal.
//
// The gate is load-bearing: a tick that overlaps an in-flight refresh is the
// duplicate-worker bug (§5), and a tick after q/Ctrl-C is a worker the quit
// path cannot unwind (§6.7).

#include <chrono>

namespace tui {

// The cadence the ticker wakes on. Named, not inlined: the interval is a
// design choice (a list is stale after a couple of seconds), and one place
// changes it.
inline constexpr std::chrono::milliseconds kAutoRefreshInterval{2500};

struct AutoRefreshState {
  bool on_owning_tab = false;  // the active tab is the one that owns the list
  bool in_flight = false;      // a refresh/action for that list is running
  bool shutting_down = false;  // q/Ctrl-C: never spawn another worker
  std::chrono::milliseconds since_last{0};
};

// True only when enough time has passed, the owning tab is active, nothing is
// in flight and the panel is not shutting down. Every false case is a worker
// that is not spawned.
[[nodiscard]] bool auto_refresh_due(const AutoRefreshState& state);

}  // namespace tui
