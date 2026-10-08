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

std::vector<std::string> services_table_lines(
    const std::vector<berth::ServiceRow>& rows) {
  std::vector<std::string> lines;
  lines.reserve(rows.size() + 1);

  std::string head;
  pad_to(head, "SERVICE", kNameWidth);
  pad_to(head, "STATE", kStateWidth);
  pad_to(head, "PORT", kPortWidth);
  head.append("COMMIT");
  lines.push_back(std::move(head));

  for (const berth::ServiceRow& row : rows) {
    std::string line;
    pad_to(line, row.service_name, kNameWidth);
    pad_to(line, berth::service_state_label(row.state), kStateWidth);
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

std::vector<std::string> services_key_hints() {
  return {
      "s start   x stop   u uninstall   r refresh   Enter refresh",
      "status comes from `systemctl --user is-active`, one bounded call per "
      "row, on a worker.",
  };
}

}  // namespace tui