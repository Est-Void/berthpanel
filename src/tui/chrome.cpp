// src/tui/chrome.cpp — see chrome.h for the contract.

#include "chrome.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace tui {

namespace {

constexpr std::array<TabSpec, kTabCount> kTabs{{
    {Tab::kCatalog, "@", "System Scan", "cyan"},
    {Tab::kDeploy, "^", "Deploy", "magenta"},
    {Tab::kServices, "#", "My Services", "green"},
    {Tab::kLogs, "\xe2\x89\xa1", "Logs", "yellow"},  // U+2261 IDENTICAL TO
}};

// The brand mark. Centre column 2 on all three lines — asserted by the test,
// because a shift is invisible in review and obvious on screen.
const std::vector<std::string> kSignLines{"  o", " -+-", "\\_|_/"};

constexpr std::array<FooterHint, 3> kFooterHints{{
    {"\xe2\x86\x91\xe2\x86\x93", "select", "cyan"},  // ↑↓
    {"Enter", "choose", "magenta"},
    {"q", "quit", "yellow"},
}};

}  // namespace

std::span<const TabSpec> tabs() {
  return std::span<const TabSpec>(kTabs);
}

std::size_t tab_index(const Tab tab) {
  for (std::size_t i = 0; i < kTabs.size(); ++i) {
    if (kTabs[i].tab == tab) {
      return i;
    }
  }
  std::fputs("tui: invariant violated: unknown Tab value\n", stderr);
  std::abort();
}

const std::vector<std::string>& sign_lines() {
  return kSignLines;
}

std::span<const FooterHint> footer_hints() {
  return std::span<const FooterHint>(kFooterHints);
}

std::string_view pending_notice(const Tab tab) {
  switch (tab) {
    case Tab::kDeploy:
      return "The deploy screen arrives in the next stage.\n"
             "For now use the CLI:\n"
             "  berthpanel __deploy <service_name>\n"
             "The recipe's pinned commit, resource limits and start command\n"
             "are on the Catalog screen — press Enter on a recipe to read "
             "them.";
    case Tab::kLogs:
      return "The logs screen arrives in the next stage.\n"
             "net.log is already being written for every outbound request;\n"
             "the list of allowlisted endpoints is available right now:\n"
             "  berthpanel --show-endpoints";
    case Tab::kCatalog:
    case Tab::kServices:
      // Both are built (screens.cpp). Asking for a "coming soon" notice for
      // them would mean the tab table and the render switch disagree.
      // Not abort_bug() from panel.h: chrome.cpp is linked into the TEST
      // binary, which does not link FTXUI at all (§9).
      std::fputs("tui: invariant violated: pending_notice on a built tab\n",
                 stderr);
      std::abort();
  }
  std::abort();
}

}  // namespace tui