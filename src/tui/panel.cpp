#include "panel.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/util/autoreset.hpp>

#include <algorithm>
#include <cstddef>
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
  // A closed set from chrome.h: an unknown name is a bug in that table, not a
  // runtime condition to absorb.
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

namespace {

// A viewport, not a frame: the content keeps its own height and the visible
// window moves. A vbox in the same place would shrink the rows to zero.
class ScrollView : public ftxui::Node {
 public:
  ScrollView(ftxui::Element content, std::size_t* scroll)
      : ftxui::Node({std::move(content)}), scroll_(scroll) {}

 private:
  void ComputeRequirement() override {
    ftxui::Node::ComputeRequirement();
    // The viewport absorbs the parent's height budget; the child must not.
    requirement_.min_x = 1;
    requirement_.min_y = 1;
    requirement_.flex_grow_y = 1;
    requirement_.flex_shrink_y = 1;
  }

  void SetBox(ftxui::Box box) override {
    ftxui::Node::SetBox(box);
    const int visible = std::max(0, box.y_max - box.y_min + 1);
    content_rows_ = std::max(0, children_[0]->requirement().min_y);
    max_scroll_ = content_rows_ > visible
                      ? static_cast<std::size_t>(content_rows_ - visible)
                      : 0;
    // Do NOT write the clamp back here: requirement is transient during the
    // iterations, so a stale zero would erase the user's page request. Render
    // runs once, after the layout has settled.
    offset_ = *scroll_ < max_scroll_ ? *scroll_ : max_scroll_;
    show_bar_ = max_scroll_ > 0 && box.x_max > box.x_min;

    // Give the child its natural height (never the visible one), so a short
    // terminal clips at the stencil instead of rewriting every row's size.
    ftxui::Box inner = box;
    if (show_bar_) {
      --inner.x_max;
    }
    const int inner_rows = std::max(content_rows_, visible);
    inner.y_min = box.y_min - static_cast<int>(offset_);
    inner.y_max = inner.y_min + inner_rows - 1;
    children_[0]->SetBox(inner);
  }

  void Render(ftxui::Screen& screen) override {
    {
      const ftxui::AutoReset<ftxui::Box> stencil(
          &screen.stencil, ftxui::Box::Intersection(box_, screen.stencil));
      children_[0]->Render(screen);
    }
    // The layout has settled by the one Render, so this is the safe place to
    // clamp the stored scroll for the next key press.
    *scroll_ = offset_;
    if (!show_bar_) {
      return;
    }
    const int top = box_.y_min;
    const int height = box_.y_max - top + 1;
    const int total = std::max(content_rows_, 1);
    const int thumb = std::clamp(height * height / total, 1, height);
    const int span = std::max(1, height - thumb);
    const int bar_offset =
        max_scroll_ == 0
            ? 0
            : static_cast<int>(offset_ * static_cast<std::size_t>(span) /
                               max_scroll_);
    for (int y = 0; y < height; ++y) {
      auto& pixel = screen.PixelAt(box_.x_max, top + y);
      const bool in_thumb = y >= bar_offset && y < bar_offset + thumb;
      pixel.character = in_thumb ? "\xe2\x94\x83" : "\xe2\x94\x82";
      pixel.automerge = false;
    }
  }

  std::size_t* scroll_;
  int content_rows_ = 0;
  std::size_t max_scroll_ = 0;
  std::size_t offset_ = 0;
  bool show_bar_ = false;
};

}

Element scroll_view(Element content, std::size_t* scroll) {
  return std::make_shared<ScrollView>(std::move(content), scroll);
}

Element line_list(const std::vector<std::string>& lines,
                  const std::size_t cursor, const Color accent,
                  const bool cursor_visible, const std::size_t visible) {
  const std::size_t first = window_start(lines.size(), cursor, visible);
  Elements rows;
  for (std::size_t i = first; i < lines.size() && rows.size() < visible; ++i) {
    const bool selected = cursor_visible && (i == cursor);
    // Fixed-width marker column, so every row lines up.
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

Element boxed_dialog(const std::vector<std::string>& lines,
                     const Color accent) {
  Elements body;
  for (const std::string& line : lines) {
    body.push_back(line.empty() ? ftxui::text(" ") : ftxui::paragraph(line));
  }
  Element dialog = ftxui::vbox(std::move(body)) |
                   ftxui::borderStyled(ftxui::ROUNDED, accent);
  // Centred by fillers, not absolute coordinates, so it survives a resize. The
  // dialog itself stays non-flexible: shrinking it eats the y/n lines first.
  return ftxui::vcenter(ftxui::hcenter(std::move(dialog)));
}

Element header_bar(const berth::NetSnapshot& net) {
  const std::vector<std::string>& sign = sign_lines();
  Elements lines;
  for (std::size_t i = 0; i < sign.size(); ++i) {
    std::string glyph(kSignLeftMargin, ' ');
    glyph.append(sign[i]);
    // Same total width on all three lines, so the wordmark and the tagline
    // start in the same column.
    pad_to_width(glyph, kSignLeftMargin + kSignWidth);
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
    lines.push_back(ftxui::hbox(std::move(row)));
  }

  // Deliberately SHORT: an over-long header row does not clip its tail, it
  // gets shrunk cell by cell, and a clipped count is a wrong count. The
  // sentence explaining it lives on the Logs screen, where there is room.
  const std::string banner = net_banner(net);
  if (!banner.empty()) {
    lines.push_back(ftxui::text(std::string(kSignLeftMargin, ' ') + banner) |
                    ftxui::bold | ftxui::color(Color::Yellow));
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
  rows.push_back(ftxui::text("\xe2\x86\x90\xe2\x86\x92 tab") | ftxui::dim);
  rows.push_back(ftxui::text("PgUp/PgDn scroll") | ftxui::dim);
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

Element key_hint(const KeyHint& hint) {
  if (hint.keys.empty()) {
    return ftxui::text(std::string(hint.words)) | ftxui::dim;
  }
  Elements parts;
  parts.push_back(ftxui::text(std::string(hint.keys)) | ftxui::bold |
                  ftxui::color(accent_color(kKeyAccent)));
  parts.push_back(ftxui::text("  " + std::string(hint.words)) | ftxui::dim);
  return ftxui::hbox(std::move(parts));
}

Element key_hint_line(const std::vector<KeyHint>& hints) {
  Elements parts;
  for (std::size_t i = 0; i < hints.size(); ++i) {
    if (i > 0) {
      parts.push_back(ftxui::text(" \xc2\xb7 ") | ftxui::dim);
    }
    parts.push_back(key_hint(hints[i]));
  }
  return ftxui::hbox(std::move(parts));
}

}
