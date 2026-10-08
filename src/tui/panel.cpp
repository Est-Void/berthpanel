// src/tui/panel.cpp — see panel.h for the contract.

#include "panel.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <utility>

#include "../util/strutil.h"

namespace tui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

void request_render(const Panel& panel) {
  if (panel.shutting_down.load() || panel.screen == nullptr) {
    return;
  }
  panel.screen->PostEvent(ftxui::Event::Custom);
}

void abort_bug(const char* what) {
  std::fputs("tui: invariant violated: ", stderr);
  std::fputs(what, stderr);
  std::fputc('\n', stderr);
  std::abort();
}

Color accent_color(const std::string_view name) {
  // A closed set declared in chrome.h; an unknown name is a bug in that
  // table, not a runtime condition to absorb.
  if (name == "cyan") {
    return Color::Cyan;
  }
  if (name == "magenta") {
    return Color::Magenta;
  }
  if (name == "green") {
    return Color::Green;
  }
  if (name == "yellow") {
    return Color::Yellow;
  }
  if (name == "blue") {
    return Color::Blue;
  }
  abort_bug("unknown accent color");
}

void pad_to_width(std::string& out, const std::size_t width) {
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
}

std::size_t window_start(const std::size_t count, const std::size_t cursor,
                         const std::size_t visible) {
  if (count <= visible) {
    return 0;
  }
  if (cursor < visible / 2) {
    return 0;
  }
  const std::size_t start = cursor - visible / 2;
  const std::size_t max_start = count - visible;
  return start < max_start ? start : max_start;
}

Element line_list(const std::vector<std::string>& lines,
                  const std::size_t cursor, const Color accent,
                  const bool cursor_visible, const std::size_t visible) {
  const std::size_t first = window_start(lines.size(), cursor, visible);
  Elements rows;
  for (std::size_t i = first; i < lines.size() && rows.size() < visible; ++i) {
    const bool selected = cursor_visible && (i == cursor);
    // The marker column is a fixed width so every row lines up.
    Element line = ftxui::text((selected ? "\xc2\xbb " : "  ") + lines[i]);
    if (selected) {
      line |= ftxui::bold | ftxui::color(accent);
    } else {
      line |= ftxui::dim;
    }
    rows.push_back(std::move(line));
  }
  return ftxui::vbox(std::move(rows)) | ftxui::flex;
}

Element paragraph_lines(const std::vector<std::string>& lines) {
  Elements out;
  for (const std::string& line : lines) {
    out.push_back(line.empty() ? ftxui::text(" ") : ftxui::paragraph(line));
  }
  return ftxui::vbox(std::move(out));
}

Element header_bar(const berth::NetSnapshot& net) {
  const std::vector<std::string>& sign = sign_lines();
  Elements lines;
  for (std::size_t i = 0; i < sign.size(); ++i) {
    // All three sign lines get the same total width, so the wordmark and the
    // tagline start in the same column.
    std::string glyph = sign[i];
    pad_to_width(glyph, kSignWidth);
    glyph.append(kSignGap, ' ');

    Elements row;
    row.push_back(ftxui::text(std::move(glyph)) | ftxui::color(Color::Cyan));
    if (i == 0) {
      row.push_back(ftxui::text(std::string(kProductName)) | ftxui::bold |
                    ftxui::color(Color::Cyan));
      row.push_back(ftxui::text(std::string(kProductSuffix)) | ftxui::dim);
    } else if (i == 1) {
      row.push_back(ftxui::text(tagline_line()) | ftxui::dim);
    }
    // Line 2 (the beacon) carries nothing but the mark, so it reads as a
    // light over water sitting under the wordmark.
    lines.push_back(ftxui::hbox(std::move(row)));
  }

  // The NET banner hangs under the header. Empty when nothing has been
  // requested, so the header does not carry a row of zeroes (§6.1: the number
  // is a claim, and a claim about zero requests is noise). It is deliberately
  // SHORT: an over-long header row does not clip its tail, it gets shrunk cell
  // by cell, and a clipped count is a wrong count. The sentence explaining it
  // lives on the Logs screen, where there is room for it.
  const std::string banner = net_banner(net);
  if (!banner.empty()) {
    lines.push_back(ftxui::text("  " + banner) | ftxui::bold |
                    ftxui::color(Color::Yellow));
  }
  return ftxui::vbox(std::move(lines)) |
         ftxui::borderStyled(ftxui::ROUNDED, Color::Cyan);
}

Element tab_strip(const std::size_t active) {
  Elements rows;
  const std::span<const TabSpec> specs = tabs();
  for (std::size_t i = 0; i < specs.size(); ++i) {
    const TabSpec& spec = specs[i];
    const bool selected = (i == active);
    const Color accent = accent_color(spec.accent);

    Element marker = ftxui::text(selected ? "\xc2\xbb " : "  ");
    if (selected) {
      marker |= ftxui::bold | ftxui::color(accent);
    } else {
      marker |= ftxui::dim;
    }
    // The glyph keeps its accent either way — it is how a tab is recognized at
    // a glance.
    Element glyph = ftxui::text(std::string(spec.glyph)) | ftxui::color(accent);
    Element label = ftxui::text(std::string(spec.label));
    if (selected) {
      label |= ftxui::bold | ftxui::color(accent);
    } else {
      label |= ftxui::dim;
    }
    rows.push_back(ftxui::hbox({std::move(marker), std::move(glyph),
                                ftxui::text(" "), std::move(label)}));
  }
  rows.push_back(ftxui::text(""));
  rows.push_back(ftxui::text("\xe2\x86\x90\xe2\x86\x92 tab") | ftxui::dim);
  return ftxui::vbox(std::move(rows)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL,
                     static_cast<float>(kTabsWidth));
}

Element footer_bar() {
  Elements parts;
  const std::span<const FooterHint> hints = footer_hints();
  for (std::size_t i = 0; i < hints.size(); ++i) {
    if (i > 0) {
      parts.push_back(ftxui::text(" \xc2\xb7 ") | ftxui::dim);
    }
    parts.push_back(ftxui::text(std::string(hints[i].keys)) | ftxui::bold |
                    ftxui::color(accent_color(hints[i].accent)));
    parts.push_back(ftxui::text(" " + std::string(hints[i].words)) |
                    ftxui::dim);
  }
  return ftxui::hbox(std::move(parts));
}

}  // namespace tui