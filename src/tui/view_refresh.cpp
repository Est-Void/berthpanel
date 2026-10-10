// src/tui/view_refresh.cpp — see view_refresh.h for the contract.

#include "view_refresh.h"

namespace tui {

bool auto_refresh_due(const AutoRefreshState& state) {
  if (state.shutting_down || state.in_flight || !state.on_owning_tab) {
    return false;
  }
  return state.since_last >= kAutoRefreshInterval;
}

}  // namespace tui
