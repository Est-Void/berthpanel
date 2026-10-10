#pragma once

// src/tui/view_services.h — the My Services view model (plan stage 6.4) as
// pure functions over berth::ServiceRow. No FTXUI, no threads: the engine
// call that fills the rows happens on a worker (service_ops.h forbids it on
// the render thread), and everything the screen actually draws comes from
// here.
//
// The rows are only ever refreshed on an explicit key (Enter or r), never in
// the render loop — a render loop that polls `systemctl` is the old bug
// (dev.md §11).

#include <string>
#include <vector>

#include "../berth/service_ops.h"
#include "../util/result.h"
#include "chrome.h"  // KeyHint: the shared keys+words structured hint

namespace tui {

// Where the My Services cursor belongs after `rows` was replaced by a refresh.
//
// The cursor is a SLOT in the table, and a refresh changes the slots: dropping
// the selected service, or inserting one above it, silently re-points the
// highlight — and the highlight is what the next key press (u, K, s) acts on.
// A session in this project did exactly that: a confirm dialog named
// berthpanel-memos while the highlight had already moved to berthpanel-ascii-live,
// so the screen offered to delete a service the user was no longer looking at.
//
// So the cursor follows the SERVICE it was on. Only a service that is gone from
// the list moves it, and then only as far as the old index still reaches.
[[nodiscard]] std::size_t cursor_after_rows_refresh(
    std::size_t cursor, const std::vector<berth::ServiceRow>& old_rows,
    const std::vector<berth::ServiceRow>& new_rows);


// The table: a NON-SELECTABLE header plus one selectable line per row, returned
// separately for the same reason as catalog_header_line()/catalog_row_lines():
// the cursor indexes `rows`, so feeding line_list() a vector with a header at 0
// made the highlight — and the key that acts — point one row too high.
[[nodiscard]] std::string services_header_line();
[[nodiscard]] std::vector<std::string> services_row_lines(
    const std::vector<berth::ServiceRow>& rows);

// Shown instead of the table when the registry is empty.
[[nodiscard]] std::vector<std::string> services_empty_lines();

// The kill confirmation: SIGKILL is irreversible for whatever the process had
// not flushed, and the dialog says what that costs rather than just asking.
[[nodiscard]] std::vector<std::string> confirm_kill_lines(
    const berth::ServiceRow& row);

// The uninstall confirmation. It MUST name what stays behind: the work dir
// and the user's data are not removed, and a confirmation that hides that is
// a confirmation for a different operation (§11 — silent data deletion is
// forbidden, and so is hiding it behind a dialog).
[[nodiscard]] std::vector<std::string> confirm_uninstall_lines(
    const berth::ServiceRow& row);

// One line per group naming the keys this screen acts on. Structured as
// keys+words so the renderer colours only the keys.
[[nodiscard]] std::vector<KeyHint> services_key_hints();

}  // namespace tui