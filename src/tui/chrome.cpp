// src/tui/chrome.cpp — see chrome.h for the contract.

#include "chrome.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "version.h"

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

std::string tagline_line() {
  // BERTH_VERSION is `git describe` output: it carries its own "v" when the
  // build has a tag, so the prefix supplies only the separator.
  return std::string(kTaglinePrefix) + BERTH_VERSION;
}

}  // namespace tui