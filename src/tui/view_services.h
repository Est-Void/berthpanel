#pragma once

#include <string>
#include <vector>

#include "../berth/service_ops.h"
#include "../util/result.h"
#include "chrome.h"  // KeyHint: the shared keys+words structured hint

namespace tui {

// The cursor follows the SERVICE it was on: a refresh changes the slots, and
// the highlight is what the next key press (u, K, s) acts on.
[[nodiscard]] std::size_t cursor_after_rows_refresh(
    std::size_t cursor, const std::vector<berth::ServiceRow>& old_rows,
    const std::vector<berth::ServiceRow>& new_rows);

// The header is NOT selectable: the cursor indexes `rows`, so one vector for
// both would point the highlight — and the key that acts — one row too high.
[[nodiscard]] std::string services_header_line();
[[nodiscard]] std::vector<std::string> services_row_lines(
    const std::vector<berth::ServiceRow>& rows);

[[nodiscard]] std::vector<std::string> services_empty_lines();

[[nodiscard]] std::vector<std::string> confirm_kill_lines(
    const berth::ServiceRow& row);

// Must name what stays behind: hiding the retained data is §11 silent deletion.
[[nodiscard]] std::vector<std::string> confirm_uninstall_lines(
    const berth::ServiceRow& row);

[[nodiscard]] std::vector<KeyHint> services_key_hints();

}
