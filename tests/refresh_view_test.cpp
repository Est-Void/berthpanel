// Tests for the auto-refresh gate (stage 6.4/6.2): the pure "should I refresh
// now" decision the TUI ticker asks before it spawns anything. The timer
// itself needs a terminal and is not tested here — the rule that a tick never
// overlaps an in-flight refresh and never runs after q/Ctrl-C is.

#include "tui/view_refresh.h"

#include <chrono>

#include "runner.h"

using namespace std::chrono_literals;

TEST(auto_refresh_is_due_only_after_the_interval_on_the_owning_tab_idle) {
  tui::AutoRefreshState state;
  state.on_owning_tab = true;
  state.since_last = tui::kAutoRefreshInterval;
  CHECK(tui::auto_refresh_due(state));

  state.since_last = tui::kAutoRefreshInterval + 1ms;
  CHECK(tui::auto_refresh_due(state));

  // Too soon: the previous tick was less than the cadence ago.
  state.since_last = tui::kAutoRefreshInterval - 1ms;
  CHECK(!tui::auto_refresh_due(state));

  // The owning tab is not active: no worker for a list nobody is looking at.
  state.since_last = tui::kAutoRefreshInterval;
  state.on_owning_tab = false;
  CHECK(!tui::auto_refresh_due(state));
}

TEST(auto_refresh_never_overlaps_and_never_spawns_on_shutdown) {
  tui::AutoRefreshState state;
  state.on_owning_tab = true;
  state.since_last = tui::kAutoRefreshInterval;

  // A refresh for this list is already in flight: a second tick must not
  // spawn a duplicate worker over the same state.
  state.in_flight = true;
  CHECK(!tui::auto_refresh_due(state));
  state.in_flight = false;

  // q / Ctrl-C must unwind what is running: no NEW worker after shutdown.
  state.shutting_down = true;
  CHECK(!tui::auto_refresh_due(state));
}
