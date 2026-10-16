#include "stacks.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "../core/names.h"

namespace berth {

Result<std::string> member_unit_name(const std::string_view group,
                                     const std::string_view member) {
  if (!validate_unit_name(group)) {
    return Error("stacks: group service_name '" + std::string(group) +
                 "' does not pass validate_unit_name");
  }
  if (member.empty()) {
    return Error("stacks: a stack member needs a non-empty name");
  }
  std::string name = std::string(group) + "-" + std::string(member);
  if (!validate_unit_name(name)) {
    return Error("stacks: member name '" + std::string(member) +
                 "' derives an invalid unit name '" + name + "'");
  }
  return name;
}

namespace {

bool contains_member(const std::vector<StackMember>& members,
                     const std::string_view name) {
  return std::any_of(members.begin(), members.end(),
                     [name](const StackMember& m) { return m.name == name; });
}

std::size_t member_index(const std::vector<StackMember>& members,
                         const std::string_view name) {
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (members[i].name == name)
      return i;
  }
  return members.size();
}

// Kahn over after ∪ wants. A node that never leaves the queue is in a cycle;
// the caller must have proven every reference is a known sibling first.
bool topological_order(const std::vector<StackMember>& members,
                       std::vector<std::size_t>* order) {
  const std::size_t n = members.size();
  std::vector<std::vector<std::size_t>> outgoing(n);
  std::vector<std::size_t> indegree(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    std::vector<bool> seen(n, false);
    const auto add_edge = [&](const std::string& dep) {
      const std::size_t d = member_index(members, dep);
      if (d < n && !seen[d]) {
        seen[d] = true;
        outgoing[d].push_back(i);
        ++indegree[i];
      }
    };
    for (const std::string& dep : members[i].after)
      add_edge(dep);
    for (const std::string& dep : members[i].wants)
      add_edge(dep);
  }
  std::vector<std::size_t> ready;
  for (std::size_t i = 0; i < n; ++i) {
    if (indegree[i] == 0)
      ready.push_back(i);
  }
  std::vector<std::size_t> resolved;
  resolved.reserve(n);
  for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
    const std::size_t node = ready[cursor];
    resolved.push_back(node);
    for (const std::size_t next : outgoing[node]) {
      if (--indegree[next] == 0)
        ready.push_back(next);
    }
  }
  if (resolved.size() != n)
    return false;
  *order = std::move(resolved);
  return true;
}

Result<std::vector<std::string>> member_refs_from_json(
    const nlohmann::json& entry, const char* field) {
  std::vector<std::string> refs;
  if (!entry.contains(field))
    return refs;
  const nlohmann::json& list = entry.at(field);
  if (!list.is_array()) {
    return Error(std::string("stacks: member.") + field + " must be an array");
  }
  for (const nlohmann::json& item : list) {
    if (!item.is_string()) {
      return Error(std::string("stacks: member.") + field +
                   " entries must be member names");
    }
    refs.push_back(item.get<std::string>());
  }
  return refs;
}

}

Result<void> validate_stack(const Stack& stack) {
  if (!validate_unit_name(stack.service_name)) {
    return Error("stacks: invalid group service_name '" + stack.service_name +
                 "'");
  }
  if (stack.members.size() < 2) {
    return Error("stacks: '" + stack.service_name +
                 "' needs at least two members (a one-member stack is a "
                 "single unit)");
  }
  std::vector<std::string> names;
  for (const StackMember& member : stack.members) {
    Result<std::string> derived =
        member_unit_name(stack.service_name, member.name);
    if (derived.is_err())
      return std::move(derived).error();
    if (member.recipe.service_name != derived.value()) {
      return Error("stacks: member '" + member.name + "' service_name must be '" +
                   derived.value() + "', got '" + member.recipe.service_name +
                   "'");
    }
    if (std::find(names.begin(), names.end(), member.name) != names.end()) {
      return Error("stacks: duplicate member name '" + member.name + "'");
    }
    names.push_back(member.name);
    Result<void> valid = validate_recipe(member.recipe);
    if (valid.is_err()) {
      return Error("stacks: member '" + member.name +
                   "': " + valid.error().ctx());
    }
  }
  for (const StackMember& member : stack.members) {
    for (const std::string& dep : member.after) {
      if (dep == member.name)
        return Error("stacks: member '" + member.name +
                     "' cannot depend on itself");
      if (!contains_member(stack.members, dep))
        return Error("stacks: member '" + member.name +
                     "' references unknown sibling '" + dep + "'");
    }
    for (const std::string& dep : member.wants) {
      if (dep == member.name)
        return Error("stacks: member '" + member.name +
                     "' cannot depend on itself");
      if (!contains_member(stack.members, dep))
        return Error("stacks: member '" + member.name +
                     "' references unknown sibling '" + dep + "'");
    }
  }
  std::vector<std::size_t> ignored;
  if (!topological_order(stack.members, &ignored)) {
    return Error("stacks: '" + stack.service_name +
                 "' has a member dependency cycle");
  }
  return {};
}

Result<std::vector<std::size_t>> stack_deploy_order(const Stack& stack) {
  std::vector<std::size_t> order;
  if (!topological_order(stack.members, &order)) {
    return Error("stacks: '" + stack.service_name +
                 "' has a member dependency cycle");
  }
  return order;
}

Result<Stack> stack_from_json(const nlohmann::json& doc) {
  if (!doc.is_object())
    return Error("stacks: not an object");
  if (!doc.contains("members"))
    return Error("stacks: missing members array");
  const nlohmann::json& members = doc.at("members");
  if (!members.is_array())
    return Error("stacks: members must be an array");
  Stack stack;
  stack.service_name = doc.value("service_name", "");
  stack.display_name = doc.value("display_name", "");
  stack.description = doc.value("description", "");
  for (const nlohmann::json& entry : members) {
    if (!entry.is_object())
      return Error("stacks: each member must be an object");
    StackMember member;
    member.name = entry.value("member", "");
    Result<std::vector<std::string>> after =
        member_refs_from_json(entry, "after");
    if (after.is_err())
      return std::move(after).error();
    member.after = std::move(after).value();
    Result<std::vector<std::string>> wants =
        member_refs_from_json(entry, "wants");
    if (wants.is_err())
      return std::move(wants).error();
    member.wants = std::move(wants).value();
    Result<Recipe> recipe = recipe_from_json(entry);
    if (recipe.is_err())
      return std::move(recipe).error();
    member.recipe = std::move(recipe).value();
    stack.members.push_back(std::move(member));
  }
  return stack;
}

nlohmann::json stack_to_json(const Stack& stack) {
  nlohmann::json members = nlohmann::json::array();
  for (const StackMember& member : stack.members) {
    nlohmann::json entry = recipe_to_json(member.recipe);
    entry["member"] = member.name;
    // Absent stays absent, so a stack file does not churn on re-store.
    if (!member.after.empty())
      entry["after"] = member.after;
    if (!member.wants.empty())
      entry["wants"] = member.wants;
    members.push_back(std::move(entry));
  }
  nlohmann::json doc;
  doc["service_name"] = stack.service_name;
  doc["display_name"] = stack.display_name;
  doc["description"] = stack.description;
  doc["members"] = std::move(members);
  return doc;
}

}
