#pragma once

// Multi-unit recipes (stacks, §8): a group stem plus a members[] of complete
// §8 recipes. The single-unit validator stays the per-member contract; this
// module adds ONLY the group rules (derived unit names, unique member ids,
// sibling after/wants, acyclicity). A one-member set is a single unit, not a
// stack, so two is the floor.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include "../util/result.h"
#include "recipes.h"

namespace berth {

struct StackMember {
  std::string name;                // short id within the group: "app", "db"
  Recipe recipe;                   // a complete single-unit §8 recipe
  std::vector<std::string> after;  // sibling member names
  std::vector<std::string> wants;  // sibling member names
};

struct Stack {
  std::string service_name;  // group stem, berthpanel-<group>
  std::string display_name;
  std::string description;
  std::vector<StackMember> members;
};

// group + "-" + member, through the ONE validate_unit_name (§11). An Err names
// which part is illegal rather than returning a name cleanup cannot own.
[[nodiscard]] Result<std::string> member_unit_name(std::string_view group,
                                                   std::string_view member);

[[nodiscard]] Result<void> validate_stack(const Stack& stack);

// Member indices so that a member follows every sibling it lists in
// after/wants. A cycle (already rejected by validate_stack) is an Err.
[[nodiscard]] Result<std::vector<std::size_t>> stack_deploy_order(
    const Stack& stack);

[[nodiscard]] Result<Stack> stack_from_json(const nlohmann::json& doc);
[[nodiscard]] nlohmann::json stack_to_json(const Stack& stack);

}
