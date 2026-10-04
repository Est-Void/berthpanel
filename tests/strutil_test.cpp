// Tests for util/strutil.h (stage 1.2): explicit-delimiter split/join
// round-trips, empty-field semantics, ASCII trim, exact prefix/suffix.

#include "util/strutil.h"

#include <string>
#include <vector>

#include "runner.h"

using berth::ends_with;
using berth::join;
using berth::split;
using berth::starts_with;
using berth::trim;

TEST(split_keeps_empty_fields) {
  const std::vector<std::string> parts = split("a,,b", ",");
  CHECK(parts.size() == 3);
  CHECK(parts[0] == "a");
  CHECK(parts[1].empty());
  CHECK(parts[2] == "b");
}

TEST(split_never_treats_whitespace_as_delimiter) {
  const std::vector<std::string> parts =
      split("ExecStart=/bin/app --flag x", "=");
  CHECK(parts.size() == 2);
  CHECK(parts[0] == "ExecStart");
  CHECK(parts[1] == "/bin/app --flag x");
}

TEST(split_multichar_delim_and_trailing) {
  const std::vector<std::string> parts = split("a::b::", "::");
  CHECK(parts.size() == 3);
  CHECK(parts[0] == "a");
  CHECK(parts[1] == "b");
  CHECK(parts[2].empty());
}

TEST(split_without_delim_is_single_field) {
  const std::vector<std::string> parts = split("solo", ",");
  CHECK(parts.size() == 1);
  CHECK(parts[0] == "solo");
}

TEST(split_empty_text_is_single_empty_field) {
  const std::vector<std::string> parts = split("", ",");
  CHECK(parts.size() == 1);
  CHECK(parts[0].empty());
}

TEST(join_round_trips_split) {
  const std::string text = "ExecStart=/bin/berth app --port 8080";
  CHECK(join(split(text, "="), "=") == text);
}

TEST(join_empty_parts_is_empty) {
  CHECK(join({}, ",").empty());
}

TEST(join_single_part_has_no_delimiter) {
  CHECK(join({"only"}, ", ") == "only");
}

TEST(trim_strips_ascii_whitespace) {
  CHECK(trim("  x\t\n") == "x");
  CHECK(trim("\r\n\t x \v\f") == "x");
}

TEST(trim_empty_and_all_whitespace) {
  CHECK(trim("").empty());
  CHECK(trim(" \t\r\n\v\f").empty());
}

TEST(trim_preserves_interior_whitespace) {
  CHECK(trim(" a b ") == "a b");
}

TEST(starts_with_exact_prefix) {
  CHECK(starts_with("berthpanel-memos.service", "berthpanel-"));
  CHECK(!starts_with("evil-berthpanel-x", "berthpanel-"));
  CHECK(!starts_with("unit", "berthpanel-"));
  CHECK(starts_with("anything", ""));
}

TEST(ends_with_exact_suffix) {
  CHECK(ends_with("file.json", ".json"));
  CHECK(!ends_with("file.json.bak", ".json"));
  CHECK(!ends_with("", "a"));
  CHECK(ends_with("", ""));
}
