#pragma once

// All three sign lines share centre column 2; a test pins the alignment.
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tui {

// No placeholder tabs: every tab here is BUILT (dev.md §11). A test pins the
// order.
enum class Tab {
  kCatalog,
  kDeploy,
  kServices,
  kLogs,
};

inline constexpr std::size_t kTabCount = 4;

inline constexpr int kTabsWidth = 22;

// The margin lives in the renderer; the test pins sign_lines()'s own columns.
inline constexpr std::size_t kSignLeftMargin = 2;

struct TabSpec {
  Tab tab;
  std::string_view glyph;
  std::string_view label;
  std::string_view accent;  // a named FTXUI color, so this header stays free
};

[[nodiscard]] std::span<const TabSpec> tabs();
[[nodiscard]] std::size_t tab_index(Tab tab);

[[nodiscard]] const std::vector<std::string>& sign_lines();

// The ONE accent every key token is drawn in; the words stay dim.
inline constexpr std::string_view kKeyAccent = "cyan";

// keys/words is DATA, so a hint is assertable without a terminal (§12.14).
struct KeyHint {
  std::string_view keys;
  std::string_view words;
};

struct FooterHint {
  std::string_view keys;
  std::string_view words;
  std::string_view accent;
};

[[nodiscard]] std::span<const FooterHint> footer_hints();

// No "v" before BERTH_VERSION: `git describe` already prints one when the
// build has a tag (§11: text claims drift from code).
inline constexpr std::string_view kTaglinePrefix =
    "self-hosting · local-only · ";
// A function, so the exact text is assertable in the test binary.
[[nodiscard]] std::string tagline_line();

inline constexpr std::string_view kProductSuffix = " · deploy panel";
inline constexpr std::string_view kProductName = "BERTHPANEL";

}
