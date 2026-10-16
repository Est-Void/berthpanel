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

constexpr std::array<FooterHint, 3> kFooterHints{{
    {"\xe2\x86\x91\xe2\x86\x93", "select", kKeyAccent},  // ↑↓
    {"Enter", "choose", kKeyAccent},
    {"q", "quit", kKeyAccent},
}};

}
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
  // Function-local static: allocation on first use, so failure throws here
  // rather than in a global initializer.
  static const std::vector<std::string> lines{"  o", " -+-", "\\_|_/"};
  return lines;
}

std::span<const FooterHint> footer_hints() {
  return std::span<const FooterHint>(kFooterHints);
}

std::string tagline_line() {
  // BERTH_VERSION is `git describe` output and carries its own "v" when the
  // build has a tag; the prefix supplies only the separator.
  return std::string(kTaglinePrefix) + BERTH_VERSION;
}

}
