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

namespace tui {

// The table: name, state, port, short commit. Fixed columns, one header.
[[nodiscard]] std::vector<std::string> services_table_lines(
    const std::vector<berth::ServiceRow>& rows);

// Shown instead of the table when the registry is empty.
[[nodiscard]] std::vector<std::string> services_empty_lines();

// The uninstall confirmation. It MUST name what stays behind: the work dir
// and the user's data are not removed, and a confirmation that hides that is
// a confirmation for a different operation (§11 — silent data deletion is
// forbidden, and so is hiding it behind a dialog).
[[nodiscard]] std::vector<std::string> confirm_uninstall_lines(
    const berth::ServiceRow& row);

// One line naming the keys this screen acts on.
[[nodiscard]] std::vector<std::string> services_key_hints();

}  // namespace tui