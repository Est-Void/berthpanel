#include "tui/text_width.h"

#include <string>
#include <string_view>

#include "runner.h"

namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

bool starts_with_continuation(const std::string& text,
                              const std::size_t offset) {
  if (offset >= text.size()) {
    return false;
  }
  const auto byte = static_cast<unsigned char>(text[offset]);
  return (byte & 0xC0U) == 0x80U;
}

}
TEST(text_width_cyrillic_is_one_column_per_codepoint) {
  const std::string privet =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
  CHECK(privet.size() == 12);
  CHECK(tui::display_width(privet) == 6);
  CHECK(tui::display_width(privet) == privet.size() / 2);
}

TEST(text_width_cjk_is_two_columns_per_glyph) {
  CHECK(tui::display_width("\xE4\xB8\xAD\xE6\x96\x87") == 4);  // 中文
  CHECK(tui::display_width("\xE4\xB8\x80") == 2);              // 一
  CHECK(tui::display_width("a\xE4\xB8\xAD" "b") == 4);
}

TEST(text_width_combining_mark_is_zero_columns) {
  CHECK(tui::display_width("e\xCC\x81") == 1);        // e + U+0301
  CHECK(tui::display_width("e\xCC\x81" "e\xCC\x81") == 2);
}

TEST(text_width_invalid_bytes_count_as_one_column_each) {
  CHECK(tui::display_width("a\xFF") == 2);
  CHECK(tui::display_width("\xE4\xB8") == 2);
}

TEST(text_width_truncate_respects_the_column_budget) {
  const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95";
  CHECK(tui::display_width(cjk) == 8);
  const std::string cut = tui::truncate_to_width(cjk, 5);
  CHECK(cut == "\xE4\xB8\xAD\xE6\x96\x87" + std::string(kEllipsis));
  CHECK(tui::display_width(cut) == 5);
}

TEST(text_width_truncate_never_splits_a_codepoint) {
  const std::string privet =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
  const std::string cut = tui::truncate_to_width(privet, 4);
  const std::string prefix =
      cut.substr(0, cut.size() - kEllipsis.size());
  CHECK(prefix == "\xD0\x9F\xD1\x80\xD0\xB8");  // При
  CHECK(privet.compare(0, prefix.size(), prefix) == 0);
  CHECK(!starts_with_continuation(privet, prefix.size()));
}

TEST(text_width_ellipsis_appears_only_when_truncating) {
  CHECK(tui::truncate_to_width("abc", 3) == "abc");
  CHECK(tui::truncate_to_width("abc", 4) == "abc");
  CHECK(tui::truncate_to_width("abcd", 4) == "abcd");
  CHECK(tui::truncate_to_width("abcdef", 4) ==
        "abc" + std::string(kEllipsis));
  CHECK(tui::truncate_to_width("abcdef", 4).find(kEllipsis) !=
        std::string::npos);
  CHECK(tui::truncate_to_width("abcd", 4).find(kEllipsis) ==
        std::string::npos);
}
