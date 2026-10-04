#pragma once

// src/berth/validate.h — the build-command guard (dev.md §5, §8): a
// denylist of dangerous patterns. The patterns are code, compiled ONCE
// with a loud abort on failure — a guard that cannot fail is not a guard
// (§11). Inputs are argv arrays (§8: build steps are argv, not shell
// strings); each element is scanned, plus the space-joined line for
// cross-argument shapes like ["rm", "-rf", "/"]. The join exists ONLY for
// matching — commands are never rebuilt from it (§11).
//
// This is a blast-radius reducer sitting in front of the sandbox, not a
// trust boundary: hostile repositories are untrusted code either way.

#include <string>
#include <string_view>
#include <vector>

#include "../util/result.h"

namespace berth {

// Rule names matched by the denylist, empty when the command is clean.
[[nodiscard]] std::vector<std::string> matched_dangerous_patterns(
    const std::vector<std::string>& argv);

[[nodiscard]] std::vector<std::string> matched_dangerous_patterns(
    std::string_view line);

// Test hook: the number of compiled rules. Zero would mean an empty
// guard, which is itself a bug (§11).
[[nodiscard]] std::size_t denylist_size();

// The ONE owner/repo and commit-SHA rules (§6.6: commits are pinned, a
// branch is never acceptable). rawfetch and gitclone share these; two
// validators that disagree is exactly the §11 trap.
[[nodiscard]] Result<void> validate_owner_repo(std::string_view owner_repo);
[[nodiscard]] Result<void> validate_commit_sha(std::string_view sha);

}  // namespace berth
