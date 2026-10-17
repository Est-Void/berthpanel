#include "berth/catalog.h"

#include <string>
#include <vector>

#include "berth/net/fetch.h"
#include "berth/recipes.h"
#include "berth/stacks.h"
#include "berth/validate.h"
#include "runner.h"

using berth::Recipe;
using berth::Stack;
using berth::StackMember;

namespace {

const StackMember* member_named(const Stack& stack, const std::string& name) {
  for (const StackMember& member : stack.members) {
    if (member.name == name)
      return &member;
  }
  return nullptr;
}

berth::Stack pilot() {
  const auto stack = berth::builtin_stack("berthpanel-gitea-stack");
  CHECK(stack.is_ok());
  return stack.value();
}

}

TEST(stack_catalog_every_builtin_stack_passes_the_validator) {
  const auto stacks = berth::builtin_stacks();
  CHECK(stacks.is_ok());
  CHECK(!stacks.value().empty());
  for (const Stack& stack : stacks.value()) {
    CHECK(berth::validate_stack(stack).is_ok());
  }
}

TEST(stack_catalog_pilot_is_an_app_plus_one_stateful_member) {
  const Stack stack = pilot();
  CHECK(stack.service_name == "berthpanel-gitea-stack");
  CHECK(stack.members.size() == 2);
  const StackMember* app = member_named(stack, "app");
  const StackMember* db = member_named(stack, "db");
  CHECK(app != nullptr);
  CHECK(db != nullptr);

  // The app is a single prebuilt binary and nothing else.
  CHECK(app->recipe.language == "prebuilt");
  CHECK(app->recipe.prebuilt.has_value());
  CHECK(app->recipe.prebuilt->archive == berth::ToolArchive::kRaw);
  CHECK(app->recipe.binary_name == "gitea");
  CHECK(app->recipe.build_steps.empty());
  CHECK(app->recipe.tools.empty());

  // Exactly one stateful member, pinned to a single extracted binary.
  CHECK(db->recipe.language == "prebuilt");
  CHECK(db->recipe.prebuilt.has_value());
  CHECK(db->recipe.prebuilt->archive == berth::ToolArchive::kTarGz);
  CHECK(db->recipe.prebuilt->member == "dragonfly-x86_64");
  CHECK(db->recipe.binary_name == "dragonfly-x86_64");
  CHECK(db->recipe.port == 16379);
  CHECK(db->recipe.start_cmd.find("--dir {project}/data") != std::string::npos);
  CHECK(db->recipe.start_cmd.find("--snapshot_cron") != std::string::npos);
}

TEST(stack_catalog_pilot_app_depends_on_the_stateful_member) {
  const Stack stack = pilot();
  const StackMember* app = member_named(stack, "app");
  CHECK(app != nullptr);
  CHECK((app->after == std::vector<std::string>{"db"}));
  CHECK((app->wants == std::vector<std::string>{"db"}));
  // Cross-member wiring is by fixed port: the schema has no per-member
  // placeholder yet.
  CHECK(app->recipe.start_cmd.find("127.0.0.1:16379") != std::string::npos);
}

TEST(stack_catalog_pilot_contract_limits_tools_loopback_pins) {
  const Stack stack = pilot();
  for (const StackMember& member : stack.members) {
    const Recipe& recipe = member.recipe;
    CHECK(!recipe.res.memory_max.empty());
    CHECK(recipe.res.tasks_max > 0);
    for (const auto& step : recipe.build_steps) {
      CHECK(berth::tool_covers_step(recipe, step));
    }
    CHECK(recipe.start_cmd.find("127.0.0.1") != std::string::npos);
    CHECK(recipe.start_cmd.find("localhost") == std::string::npos);
    CHECK(recipe.start_cmd.find("0.0.0.0") == std::string::npos);
    CHECK(berth::validate_commit_sha(recipe.commit).is_ok());
    CHECK(recipe.prebuilt.has_value());
    CHECK(recipe.prebuilt->sha256.size() == 64);
    const auto url = berth::parse_http_url(recipe.prebuilt->url);
    CHECK(url.is_ok());
    CHECK(berth::classify_endpoint(url.value().host) ==
          berth::EndpointClass::kAllowed);
  }
}

TEST(stack_catalog_pilot_has_no_nextcloud) {
  const auto stacks = berth::builtin_stacks();
  CHECK(stacks.is_ok());
  for (const Stack& stack : stacks.value()) {
    for (const StackMember& member : stack.members) {
      CHECK(member.recipe.name.find("nextcloud") == std::string::npos);
    }
  }
}
