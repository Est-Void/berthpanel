#pragma once

// src/tui/chrome.h — the panel's chrome as DATA: the tab table, the header
// sign, the footer key hints. No FTXUI here on purpose — these are the
// strings and the order, and they are testable without a terminal.
//
// The sign is three lines whose visual centres all land on the same column:
//   "  o"   -> the 'o' occupies column 2
//   " -+-"  -> occupies columns 1..3, centre 2
//   "\\_|_/" -> occupies columns 0..4, centre 2
// That is the brand mark (a light over water) and it is load-bearing: the
// tests assert the alignment, because an off-by-one here is invisible in
// code review and obvious on screen.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tui {

enum class Tab {
  kCatalog,   // @ System Scan — the catalog list; the prerequisite scan itself
              // is stage 6.5 and says so until it exists
  kDeploy,    // ^ Deploy     — stage 6.3
  kServices,  // # My Services — stage 6.4, built
  kLogs,      // ≡ Logs       — stage 6.6, with the NET banner
};

inline constexpr std::size_t kTabCount = 4;

// Fixed width of the vertical tab strip, in columns.
inline constexpr int kTabsWidth = 22;

struct TabSpec {
  Tab tab;
  std::string_view glyph;
  std::string_view label;
  std::string_view accent;  // a named FTXUI color, kept as a string so this
                            // header stays free of the rendering library
};

[[nodiscard]] std::span<const TabSpec> tabs();
[[nodiscard]] std::size_t tab_index(Tab tab);

// The three sign lines, top to bottom.
[[nodiscard]] const std::vector<std::string>& sign_lines();

// Rendered footer: keys colored, words dim. One hint per group.
struct FooterHint {
  std::string_view keys;
  std::string_view words;
  std::string_view accent;
};

[[nodiscard]] std::span<const FooterHint> footer_hints();

// Header tagline, split so the version can take its own color. No "v" in
// front of BERTH_VERSION: `git describe` already prints one when it has a
// tag, and a second one reads as "vv0.1.0" (§11: text claims drift from code).
inline constexpr std::string_view kTaglinePrefix = "self-hosting · local-only · ";
// The tagline + version, assembled. A function and not an inline expression
// so the exact text is assertable in the test binary (this header is FTXUI-free
// and the renderer is not).
[[nodiscard]] std::string tagline_line();

inline constexpr std::string_view kProductSuffix = " · deploy panel";
inline constexpr std::string_view kProductName = "BERTHPANEL";

// The exact text the not-yet-built tabs show. An honest sentence, never an
// empty box: the panel has to be able to say what it cannot do yet.
[[nodiscard]] std::string_view pending_notice(Tab tab);

}  // namespace tui