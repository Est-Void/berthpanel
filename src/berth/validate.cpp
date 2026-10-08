// src/berth/validate.cpp — see validate.h for the contract.

#include "validate.h"

#include "../util/ascii.h"

#include <algorithm>
#include <mutex>
#include <regex>
#include <string>
#include <utility>

namespace berth {

namespace {

struct DenyRule {
  const char* name;
  const char* pattern;
};

// The denylist. Any change here lands with a matching test change
// (validate_test pins shape and behavior). ECMAScript syntax.
constexpr DenyRule kDenylist[] = {
    {"sudo-or-su", R"((^|\s)(sudo|su)\s)"},
    {"pipe-to-shell", R"((curl|wget|fetch)[^|]*\|\s*(ba|z|da)?sh\b)"},
    {"rm-root", R"((^|\s)rm\s+(-[^\s]+\s+)+(/|~|\$HOME)(\s|$))"},
    {"dd-to-device", R"(dd\s+[^;|]*of=/dev/)"},
    {"mkfs", R"(mkfs(\.[a-z0-9]+)?\s)"},
    {"redirect-block-device", R"(>\s*/dev/(sd|nvme|hd|vd|disk/))"},
    {"chmod-777-root", R"(chmod\s+(-[^\s]+\s+)*777\s+/($|\s))"},
};

struct CompiledDenylist {
  std::vector<std::pair<std::string, std::regex>> rules;
};

// Compiled lazily, once; a failure to compile any pattern aborts the
// process — silently skipping a rule is the exact §11 bug.
const CompiledDenylist& compiled_denylist() {
  static CompiledDenylist compiled = [] {
    CompiledDenylist out;
    out.rules.reserve(sizeof(kDenylist) / sizeof(kDenylist[0]));
    for (const DenyRule& rule : kDenylist) {
      try {
        out.rules.emplace_back(rule.name, std::regex(rule.pattern));
      } catch (const std::regex_error& e) {
        std::fputs("berth: fatal: denylist rule failed to compile: ", stderr);
        std::fputs(rule.name, stderr);
        std::fputs(": ", stderr);
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        std::abort();
      }
    }
    return out;
  }();
  return compiled;
}

std::vector<std::string> scan_line(const std::string_view line) {
  std::vector<std::string> matched;
  const CompiledDenylist& compiled = compiled_denylist();
  for (const auto& [name, regex] : compiled.rules) {
    if (std::regex_search(line.begin(), line.end(), regex)) {
      const bool already =
          std::find(matched.begin(), matched.end(), name) != matched.end();
      if (!already)
        matched.push_back(name);
    }
  }
  return matched;
}

}  // namespace

std::vector<std::string> matched_dangerous_patterns(
    const std::vector<std::string>& argv) {
  (void)compiled_denylist();  // fail loudly before first real use
  // Each element on its own, then the joined line for cross-argument
  // shapes: ["rm", "-rf", "/"] must be caught even though no single
  // element matches (§8: argv is the currency; the join is scan-only).
  std::string joined;
  std::vector<std::string> matched;
  for (const std::string& arg : argv) {
    for (const std::string& name : scan_line(arg)) {
      if (std::find(matched.begin(), matched.end(), name) == matched.end()) {
        matched.push_back(name);
      }
    }
    if (!joined.empty())
      joined += ' ';
    joined += arg;
  }
  for (const std::string& name : scan_line(joined)) {
    if (std::find(matched.begin(), matched.end(), name) == matched.end()) {
      matched.push_back(name);
    }
  }
  return matched;
}

std::vector<std::string> matched_dangerous_patterns(
    const std::string_view line) {
  (void)compiled_denylist();
  return scan_line(line);
}

std::size_t denylist_size() {
  return compiled_denylist().rules.size();
}

Result<void> validate_owner_repo(const std::string_view owner_repo) {
  const std::size_t slash = owner_repo.find('/');
  if (slash == std::string_view::npos || slash == 0 ||
      slash + 1 >= owner_repo.size() ||
      owner_repo.find('/', slash + 1) != std::string_view::npos) {
    return Error("validate: owner/repo expected, got '" +
                 std::string(owner_repo) + "'");
  }
  if (!all_chars(owner_repo, is_repo_char)) {
    return Error("validate: bad character in '" + std::string(owner_repo) +
                 "'");
  }
  // "." and ".." pass is_repo_char (which allows '.'), but they are path
  // components, not names: "../foo" would build a URL path that walks up a
  // directory on the API host. Reject them here, at the one validator.
  const std::string_view owner = owner_repo.substr(0, slash);
  const std::string_view repo = owner_repo.substr(slash + 1);
  if (owner == "." || owner == ".." || repo == "." || repo == "..") {
    return Error("validate: '.' and '..' are not valid owner/repo components: '" +
                 std::string(owner_repo) + "'");
  }
  return {};
}

Result<void> validate_commit_sha(const std::string_view sha) {
  if (sha.size() != 40) {
    return Error("validate: commit must be a full 40-char SHA, got '" +
                 std::string(sha) + "'");
  }
  if (!all_chars(sha, is_lower_hex)) {
    return Error("validate: commit must be lowercase hex, got '" +
                 std::string(sha) + "'");
  }
  return {};
}

}  // namespace berth
