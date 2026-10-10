// src/tui/view_services.cpp — see view_services.h for the contract.

#include "view_services.h"

#include <cstddef>

#include "../core/names.h"

namespace tui {

namespace {

constexpr std::size_t kNameWidth = 26;
constexpr std::size_t kStateWidth = 10;
constexpr std::size_t kPortWidth = 8;

void pad_to(std::string& out, const std::string_view text,
            const std::size_t width) {
  out.append(text);
  if (text.size() < width) {
    out.append(width - text.size(), ' ');
  }
}

}  // namespace

std::string services_header_line() {
  std::string head;
  pad_to(head, "SERVICE", kNameWidth);
  pad_to(head, "STATE", kStateWidth);
  pad_to(head, "PORT", kPortWidth);
  head.append("COMMIT");
  return head;
}

std::vector<std::string> services_row_lines(
    const std::vector<berth::ServiceRow>& rows) {
  std::vector<std::string> lines;
  lines.reserve(rows.size());

  for (const berth::ServiceRow& row : rows) {
    std::string line;
    pad_to(line, row.service_name, kNameWidth);
    std::string state(berth::service_state_label(row.state));
    // is-active still answers "active" for a SIGSTOPped service, so the freeze
    // has to be visible here or the table claims a service is serving when it
    // is not answering at all.
    if (row.frozen && row.state == berth::ServiceState::kActive) {
      state = "frozen";
    }
    pad_to(line, state, kStateWidth);
    if (row.port == 0) {
      pad_to(line, "-", kPortWidth);
    } else {
      pad_to(line, std::to_string(row.port), kPortWidth);
    }
    line.append(row.commit_short);
    lines.push_back(std::move(line));
  }
  return lines;
}

std::vector<std::string> services_empty_lines() {
  return {
      "no services registered.",
      "",
      "switch to the Deploy tab and press Enter on a recipe,",
      "or from the CLI:",
      "  berthpanel __deploy <service_name>",
  };
}

// The kill confirmation. It MUST say what SIGKILL means — a killed service
// loses whatever it had not flushed, and it stays down unless the unit itself
// says Restart= — because a confirmation that hides the cost of saying yes is
// not a confirmation.
std::vector<std::string> confirm_kill_lines(const berth::ServiceRow& row) {
  return {
      "kill " + row.service_name + "?",
      "",
      "  SIGKILL to the service's main process, right now.",
      "",
      "  WHAT THIS COSTS:",
      "    · whatever it had not written to disk is gone — a database",
      "      mid-write does not survive this",
      "    · it does NOT come back on its own unless the unit says",
      "      Restart= (ours do not)",
      "",
      "  to make it stop answering for a while WITHOUT losing anything,",
      "  freeze it with p instead — that one is reversible.",
      "",
      "  y  yes, kill it",
      "  n  no, leave it alone",
  };
}

std::vector<std::string> confirm_uninstall_lines(const berth::ServiceRow& row) {
  return {
      "uninstall " + row.service_name + "?",
      "",
      "  stopped, disabled, unit file deleted, registry entry removed.",
      "",
      "  the work dir and its data STAY:",
      "    " + std::string(berth::kAppName) +
          " never deletes your data silently.",
      "",
      "  y  yes, uninstall",
      "  n  no, keep it",
  };
}

std::vector<KeyHint> services_key_hints() {
  return {
      {"s  x  t  p", "start · stop · restart · freeze / unfreeze"},
      {"K", "kill (SIGKILL, asks first)"},
      {"u", "uninstall"},
      {"r / Enter", "refresh"},
      {"",
       "status comes from `systemctl --user is-active`, one bounded call "
       "per row, on a worker. A frozen service still reads active to "
       "is-active, so it shows as frozen here."},
      {"",
       "freeze is SIGSTOP: the process stops answering but keeps its "
       "port — it is not a suspend."},
  };
}

std::size_t cursor_after_rows_refresh(
    const std::size_t cursor, const std::vector<berth::ServiceRow>& old_rows,
    const std::vector<berth::ServiceRow>& new_rows) {
  if (new_rows.empty()) {
    return 0;
  }
  if (cursor < old_rows.size()) {
    const std::string& anchor = old_rows[cursor].service_name;
    for (std::size_t i = 0; i < new_rows.size(); ++i) {
      if (new_rows[i].service_name == anchor) {
        return i;
      }
    }
  }
  // The selected service is gone (uninstalled, or the registry no longer lists
  // it): keep the index if it still points somewhere, else start over.
  return cursor < new_rows.size() ? cursor : 0;
}

}  // namespace tui