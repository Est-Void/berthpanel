// Tests for rawfetch.h (stage 3.7): the pure path rules, the minimal tar
// reader, and the trees API parser. Archives are assembled in-test, byte
// by byte, so every rejection case is exact. No network here.
//
// The load-bearing theme: an unsafe entry must FAIL the extraction
// loudly — never be skipped, never be partially accepted (§11).

#include "berth/rawfetch.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"

using berth::archive_path_safe;
using berth::link_target_escapes;
using berth::parse_tree_response;
using berth::tar_extract;
using berth::tar_list;

namespace {

constexpr std::size_t kBlock = 512;

void put(std::string& block, const std::size_t offset,
         const std::string& value) {
  std::memcpy(block.data() + offset, value.data(), value.size());
}

void put_octal(std::string& block, const std::size_t offset,
               const std::size_t width, const std::uint64_t value) {
  std::string field(width - 1, '0');
  std::uint64_t v = value;
  for (std::size_t i = width - 1; i > 0; --i) {
    field[i - 1] = static_cast<char>('0' + (v & 7));
    v >>= 3;
  }
  put(block, offset, field);
}

void finish_checksum(std::string& block) {
  put(block, 148, "        ");  // 8 spaces while summing
  std::uint32_t sum = 0;
  for (const char c : block) {
    sum += static_cast<std::uint8_t>(c);
  }
  put_octal(block, 148, 7, sum);  // 6 digits, then NUL + space
  block[154] = '\0';
  block[155] = ' ';
}

std::string ustar_header(const std::string& name, const std::uint64_t size,
                         const char typeflag, const std::string& link = "",
                         const std::uint32_t mode = 0644,
                         const std::string& prefix = "") {
  std::string header(kBlock, '\0');
  put(header, 0, name);
  put_octal(header, 100, 8, mode);
  put_octal(header, 108, 8, 0);
  put_octal(header, 116, 8, 0);
  put_octal(header, 124, 12, size);
  put_octal(header, 136, 12, 0);
  header[156] = typeflag;
  put(header, 157, link);
  put(header, 257, "ustar");
  header[263] = '0';
  header[264] = '0';
  put(header, 265, "root");
  put(header, 297, "root");
  if (!prefix.empty())
    put(header, 345, prefix);
  finish_checksum(header);
  return header;
}

std::string padded(const std::string& data) {
  std::string out = data;
  while (out.size() % kBlock != 0) out.push_back('\0');
  return out;
}

std::string tar_entry(const std::string& name, const std::string& content,
                      const char typeflag = '0') {
  return ustar_header(name, content.size(), typeflag) + padded(content);
}

std::string tar_end() {
  return std::string(kBlock * 2, '\0');
}

// "len key=value\n" — the length counts itself, so it must be computed
// after the digit count is known.
std::string pax_record(const std::string& key_value) {
  std::size_t length = key_value.size() + 3;  // digits + ' ' + '\n' guess
  while (true) {
    const std::size_t digits = std::to_string(length).size();
    if (digits + 1 + key_value.size() + 1 == length)
      break;
    length = digits + 1 + key_value.size() + 1;
  }
  return std::to_string(length) + " " + key_value + "\n";
}

// pax extended header ("x") carrying a path override for the next entry.
std::string pax_path_entry(const std::string& long_name,
                           const std::string& next_name,
                           const std::string& next_content) {
  const std::string record = pax_record("path=" + long_name);
  return ustar_header("PaxHeader/" + next_name, record.size(), 'x') +
         padded(record) + ustar_header(next_name, next_content.size(), '0') +
         padded(next_content);
}

}  // namespace

TEST(rawfetch_safe_paths) {
  CHECK(archive_path_safe("a/b.txt"));
  CHECK(archive_path_safe("repo-sha/"));
  CHECK(archive_path_safe("dir/./file"));
  CHECK(!archive_path_safe(""));
  CHECK(!archive_path_safe("/etc/passwd"));
  CHECK(!archive_path_safe("a/../../etc/passwd"));
  CHECK(!archive_path_safe(".."));
  CHECK(!archive_path_safe("a/.."));
  CHECK(!archive_path_safe(std::string("a\0b", 3)));
}

TEST(rawfetch_link_target_escape_rules) {
  CHECK(!link_target_escapes("a/b", "../sibling"));   // stays inside
  CHECK(!link_target_escapes("a/b", "../../c"));      // climbs to root
  CHECK(!link_target_escapes("a/b", "c/d"));          // downward
  CHECK(link_target_escapes("a/b", "../../../etc"));  // climbs out
  CHECK(link_target_escapes("", "../etc"));           // climbs out
  CHECK(link_target_escapes("a", "/etc/passwd"));     // absolute
  CHECK(link_target_escapes("a/b", ""));              // empty target
}

TEST(rawfetch_tar_lists_file_and_dir) {
  const std::string archive = ustar_header("repo-sha/", 0, '5') +
                              tar_entry("repo-sha/main.go", "package main") +
                              tar_end();
  const auto listed = tar_list(archive);
  CHECK(listed.is_ok());
  CHECK(listed.value().size() == 2);
  CHECK(listed.value()[0].name == "repo-sha/");
  CHECK(listed.value()[0].typeflag == '5');
  CHECK(listed.value()[1].name == "repo-sha/main.go");
  CHECK(listed.value()[1].size == 12);
}

TEST(rawfetch_tar_applies_pax_long_name) {
  const std::string long_name = "repo-sha/a/very/deeply/nested/path/file.bin";
  const std::string archive =
      pax_path_entry(long_name, "PaxHeader/stub", "data") + tar_end();
  const auto listed = tar_list(archive);
  CHECK(listed.is_ok());
  bool found = false;
  for (const auto& entry : listed.value()) {
    if (entry.name == long_name)
      found = true;
  }
  CHECK(found);
}

TEST(rawfetch_tar_skips_global_pax_header) {
  const std::string grecord = pax_record("comment=0123456789abcdef");
  const std::string archive =
      ustar_header("pax_global_header", grecord.size(), 'g') + padded(grecord) +
      tar_entry("repo-sha/file", "y") + tar_end();
  const auto listed = tar_list(archive);
  CHECK(listed.is_ok());
  CHECK(listed.value().size() == 1);
  CHECK(listed.value().front().name == "repo-sha/file");
}

TEST(rawfetch_tar_rejects_truncated_archives_loudly) {
  // Data cut short.
  const std::string truncated =
      ustar_header("repo-sha/big", 1000, '0') + std::string(100, 'x');
  CHECK(tar_list(truncated).is_err());

  // Header cut short.
  CHECK(tar_list(std::string(100, 'x')).is_err());

  // Valid entries but no end-of-archive terminator.
  const std::string no_end = tar_entry("repo-sha/file", "y");
  const auto listed = tar_list(no_end);
  CHECK(listed.is_err());

  // Corrupted checksum.
  std::string bad_sum =
      ustar_header("repo-sha/file", 1, '0') + padded("z") + tar_end();
  bad_sum[150] = '9';
  CHECK(tar_list(bad_sum).is_err());
}

TEST(rawfetch_tar_extract_writes_files_and_dirs) {
  const TempDir tmp;
  const std::string archive =
      ustar_header("repo-sha/", 0, '5') +
      tar_entry("repo-sha/bin/tool", "#!/bin/sh\necho hi\n", '0') +
      tar_entry("repo-sha/config.yaml", "port: 8080\n") + tar_end();

  const auto extracted = tar_extract(archive, tmp.path() / "out");
  CHECK(extracted.is_ok());
  CHECK(extracted.value() == 3);
  CHECK(std::filesystem::exists(tmp.path() / "out/repo-sha/bin/tool"));
  const auto body = berth::read_file(tmp.path() / "out/repo-sha/config.yaml");
  CHECK(body.is_ok());
  CHECK(body.value() == "port: 8080\n");
}

TEST(rawfetch_tar_extract_refuses_unsafe_paths) {
  const TempDir tmp;
  // Absolute member.
  const std::string absolute = tar_entry("/etc/passwd", "x") + tar_end();
  const auto a = tar_extract(absolute, tmp.path() / "out-a");
  CHECK(a.is_err());
  CHECK(a.error().ctx().find("unsafe") != std::string::npos);

  // Traversal member.
  const std::string traversal =
      tar_entry("repo-sha/../../escape.txt", "x") + tar_end();
  const auto t = tar_extract(traversal, tmp.path() / "out-t");
  CHECK(t.is_err());
  CHECK(!std::filesystem::exists(tmp.path() / "out-t/escape.txt"));
}

TEST(rawfetch_tar_extract_refuses_escaping_symlink_and_keeps_others) {
  const TempDir tmp;
  const std::string escaping =
      ustar_header("repo-sha/link", 0, '2', "../../etc/passwd") + tar_end();
  const auto s = tar_extract(escaping, tmp.path() / "out-s");
  CHECK(s.is_err());

  // A link that stays inside is fine.
  const std::string inside = ustar_header("repo-sha/link", 0, '2', "target") +
                             tar_entry("repo-sha/target", "data") + tar_end();
  const auto ok = tar_extract(inside, tmp.path() / "out-ok");
  CHECK(ok.is_ok());
  CHECK(std::filesystem::is_symlink(tmp.path() / "out-ok/repo-sha/link"));
}

TEST(rawfetch_tar_extract_refuses_device_entries) {
  const TempDir tmp;
  for (const char typeflag : {'3', '4', '6'}) {
    const std::string archive =
        ustar_header("repo-sha/dev", 0, typeflag) + tar_end();
    const auto r = tar_extract(archive, tmp.path() / "out-dev");
    CHECK(r.is_err());
    CHECK(r.error().ctx().find("unsupported typeflag") != std::string::npos);
  }
}

TEST(rawfetch_tar_extract_refuses_duplicate_entries) {
  const TempDir tmp;
  const std::string archive = tar_entry("repo-sha/file", "one") +
                              tar_entry("repo-sha/file", "two") + tar_end();
  const auto r = tar_extract(archive, tmp.path() / "out-dup");
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("duplicate") != std::string::npos);
}

TEST(rawfetch_trees_api_parses_blobs) {
  const std::string body = R"({
    "sha": "0123456789abcdef",
    "truncated": false,
    "tree": [
      {"path": "go.mod", "mode": "100644", "type": "blob", "sha": "aa11", "size": 42},
      {"path": "main.go", "mode": "100644", "type": "blob", "sha": "bb22", "size": 900},
      {"path": "internal", "mode": "040000", "type": "tree", "sha": "cc33"}
    ]
  })";
  const auto r = parse_tree_response(body);
  CHECK(r.is_ok());
  CHECK(r.value().size() == 2);
  CHECK(r.value()[0].path == "go.mod");
  CHECK(r.value()[0].sha == "aa11");
  CHECK(r.value()[0].size == 42);
}

TEST(rawfetch_trees_api_refuses_truncated_listing) {
  const std::string body =
      R"({"truncated": true, "tree": [{"path": "a", "type": "blob"}]})";
  const auto r = parse_tree_response(body);
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("truncated") != std::string::npos);
}

TEST(rawfetch_trees_api_rejects_junk) {
  CHECK(parse_tree_response("<html>429</html>").is_err());
  CHECK(parse_tree_response(R"({"message": "Not Found"})").is_err());
}
