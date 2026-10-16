#include "berth/stacks.h"

#include <cstddef>
#include <string>
#include <vector>

#include "berth/recipes.h"
#include "runner.h"

using berth::Recipe;
using berth::Stack;
using berth::StackMember;
using berth::validate_stack;

namespace {

constexpr const char* kCommit = "0123456789abcdef0123456789abcdef01234567";

Recipe app_recipe(const std::string& service_name) {
  Recipe r;
  r.owner = "go-gitea";
  r.name = "gitea";
  r.commit = "4ebd5e319b6a9e629bfd7a04292a6fe85b08dee9";
  r.language = "go";
  r.service_name = service_name;
  r.display_name = "Gitea";
  r.description = "the app member";
  r.build_steps = {{"go", "build", "-o", "gitea", "."}};
  r.tools = {"go"};
  r.binary_name = "gitea";
  r.start_cmd = "{bin} web --addr 127.0.0.1 --port {port}";
  r.res = {"1024M", 512};
  return r;
}

Recipe db_recipe(const std::string& service_name) {
  Recipe r;
  r.owner = "postgres";
  r.name = "postgres";
  r.commit = kCommit;
  r.language = "prebuilt";
  r.service_name = service_name;
  r.display_name = "PostgreSQL";
  r.description = "the stateful member";
  r.prebuilt = berth::PrebuiltBinary{
      .url = "https://github.com/postgres/postgres/releases/download/v1/"
             "postgres.tar.gz",
      .sha256 = std::string(64, 'a'),
      .member = "postgres"};
  r.binary_name = "postgres";
  r.start_cmd = "{self} -D {project}/data -h 127.0.0.1 -p {port}";
  r.tools = {};
  r.res = {"512M", 300};
  return r;
}

Stack good_stack() {
  Stack s;
  s.service_name = "berthpanel-demo";
  s.display_name = "Demo";
  s.description = "app + db";
  StackMember db;
  db.name = "db";
  db.recipe = db_recipe("berthpanel-demo-db");
  StackMember app;
  app.name = "app";
  app.recipe = app_recipe("berthpanel-demo-app");
  app.after = {"db"};
  app.wants = {"db"};
  s.members = {db, app};
  return s;
}

}

TEST(stacks_valid_stack_passes) {
  CHECK(validate_stack(good_stack()).is_ok());
}

TEST(stacks_member_unit_name_is_group_plus_member) {
  const auto derived = berth::member_unit_name("berthpanel-demo", "app");
  CHECK(derived.is_ok());
  CHECK(derived.value() == "berthpanel-demo-app");

  CHECK(berth::member_unit_name("berthpanel-demo", "").is_err());
  CHECK(berth::member_unit_name("no-prefix", "app").is_err());
  CHECK(berth::member_unit_name("berthpanel-demo", "app/x").is_err());
}

TEST(stacks_group_name_must_be_a_valid_unit_name) {
  Stack s = good_stack();
  s.service_name = "noprefix";
  CHECK(validate_stack(s).is_err());

  s.service_name = "berthpanel-demo";
  CHECK(validate_stack(s).is_ok());
}

TEST(stacks_needs_at_least_two_members) {
  Stack s = good_stack();
  s.members.pop_back();
  const auto one = validate_stack(s);
  CHECK(one.is_err());
  CHECK(one.error().ctx().find("two members") != std::string::npos);
}

TEST(stacks_member_service_name_must_match_the_derived_name) {
  Stack s = good_stack();
  s.members.front().recipe.service_name = "berthpanel-elsewhere";
  const auto bad = validate_stack(s);
  CHECK(bad.is_err());
  CHECK(bad.error().ctx().find("berthpanel-demo-db") != std::string::npos);
}

TEST(stacks_rejects_duplicate_member_names) {
  Stack s = good_stack();
  s.members.front().name = "app";
  s.members.front().recipe.service_name = "berthpanel-demo-app";
  CHECK(validate_stack(s).is_err());
}

TEST(stacks_contract_every_member_has_resource_limits) {
  Stack s = good_stack();
  s.members.front().recipe.res = {"", 0};
  CHECK(validate_stack(s).is_err());

  s = good_stack();
  s.members.back().recipe.res = {"512M", 0};
  CHECK(validate_stack(s).is_err());
}

TEST(stacks_contract_tools_cover_build_steps) {
  Stack s = good_stack();
  s.members.back().recipe.tools = {};  // go build, no go declared
  CHECK(validate_stack(s).is_err());
}

TEST(stacks_contract_every_member_binds_loopback_only) {
  Stack s = good_stack();
  s.members.back().recipe.start_cmd = "{bin} --addr 0.0.0.0 --port {port}";
  CHECK(validate_stack(s).is_err());

  s = good_stack();
  s.members.front().recipe.start_cmd = "{self} --port {port}";
  CHECK(validate_stack(s).is_err());  // no explicit loopback
}

TEST(stacks_contract_commits_and_pins_are_pinned) {
  Stack s = good_stack();
  s.members.back().recipe.commit = "main";
  CHECK(validate_stack(s).is_err());

  s = good_stack();
  s.members.front().recipe.prebuilt->sha256 = "short";
  CHECK(validate_stack(s).is_err());
}

TEST(stacks_rejects_unknown_sibling_references) {
  Stack s = good_stack();
  s.members.back().after = {"cache"};
  CHECK(validate_stack(s).is_err());

  s = good_stack();
  s.members.back().wants = {"missing"};
  CHECK(validate_stack(s).is_err());
}

TEST(stacks_rejects_self_dependency) {
  Stack s = good_stack();
  s.members.back().after = {"app"};
  CHECK(validate_stack(s).is_err());
}

TEST(stacks_rejects_dependency_cycles) {
  Stack s = good_stack();
  s.members.front().after = {"app"};  // db -> app while app -> db
  const auto cycle = validate_stack(s);
  CHECK(cycle.is_err());
  CHECK(cycle.error().ctx().find("cycle") != std::string::npos);
}

TEST(stacks_json_round_trip) {
  const Stack original = good_stack();
  const nlohmann::json doc = berth::stack_to_json(original);
  const auto back = berth::stack_from_json(doc);
  CHECK(back.is_ok());
  CHECK(back.value().service_name == "berthpanel-demo");
  CHECK(back.value().members.size() == 2);
  CHECK(back.value().members[1].name == "app");
  CHECK(back.value().members[1].after == std::vector<std::string>{"db"});
  CHECK(back.value().members[1].wants == std::vector<std::string>{"db"});
  CHECK(back.value().members[1].recipe.language == "go");
  CHECK(back.value().members[0].recipe.prebuilt.has_value());
  CHECK(validate_stack(back.value()).is_ok());

  // A member with no wiring keeps after/wants absent on the wire.
  Stack unwired = good_stack();
  unwired.members[1].after.clear();
  unwired.members[1].wants.clear();
  const nlohmann::json plain = berth::stack_to_json(unwired);
  CHECK(!plain.at("members")[1].contains("after"));
  CHECK(!plain.at("members")[1].contains("wants"));
}

TEST(stacks_from_json_rejects_a_non_object_and_a_missing_members_array) {
  CHECK(berth::stack_from_json(nlohmann::json::array()).is_err());
  CHECK(berth::stack_from_json(nlohmann::json::object()).is_err());
  CHECK(berth::stack_from_json(
            nlohmann::json{{"service_name", "berthpanel-demo"}})
            .is_err());
}

TEST(stacks_deploy_order_puts_a_dependency_before_its_dependent) {
  // Listed dependent-first: the order must follow after/wants, not the listing.
  Stack s = good_stack();
  s.members[0].after = {"app"};
  s.members[0].wants = {};
  s.members[1].after = {};
  s.members[1].wants = {};
  const auto order = berth::stack_deploy_order(s);
  CHECK(order.is_ok());
  CHECK((order.value() == std::vector<std::size_t>{1, 0}));
}

TEST(stacks_deploy_order_is_listing_order_without_edges) {
  Stack s = good_stack();
  s.members[1].after.clear();
  s.members[1].wants.clear();
  const auto order = berth::stack_deploy_order(s);
  CHECK(order.is_ok());
  CHECK((order.value() == std::vector<std::size_t>{0, 1}));
}
