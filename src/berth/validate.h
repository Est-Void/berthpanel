#pragma once

// The build-command guard: rules compiled ONCE with a loud abort on failure —
// a guard that cannot fail is not a guard (§11). Each argv element and the
// space-joined line are scanned; the join exists ONLY for matching.
#include <string>
#include <string_view>
#include <vector>

#include "../util/result.h"

namespace berth {

[[nodiscard]] std::vector<std::string> matched_dangerous_patterns(
    const std::vector<std::string>& argv);

[[nodiscard]] std::vector<std::string> matched_dangerous_patterns(
    std::string_view line);

// Zero rules would mean an empty guard, itself a bug (§11).
[[nodiscard]] std::size_t denylist_size();

// §6.6: commits are pinned, never a branch. One validator, shared by all.
[[nodiscard]] Result<void> validate_owner_repo(std::string_view owner_repo);
[[nodiscard]] Result<void> validate_commit_sha(std::string_view sha);

}
