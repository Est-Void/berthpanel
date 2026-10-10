// Tests for the --json documents (plan 5.3): the versioned envelope, stable
// ordering, and that the human-readable renderers are untouched.

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "berth/catalog.h"
#include "berth/userrecipes.h"
#include "core/jsonout.h"
#include "core/state.h"
#include "runner.h"

namespace {

berth::ServiceRecord record(const std::string& name, const std::uint16_t port) {
  berth::ServiceRecord r;
  r.service_name = name;
  r.display_name = name;
  r.description = "d";
  r.repo = "owner/" + name;
  r.commit = std::string(40, 'a');
  r.port = port;
  return r;
}

}  // namespace

TEST(json_services_empty_document_has_schema_and_empty_array) {
  const nlohmann::json doc = berth::services_json({});
  CHECK(doc.at("schema").get<int>() == berth::kJsonSchema);
  CHECK(doc.at("command").get<std::string>() == "services");
  CHECK(doc.at("services").is_array());
  CHECK(doc.at("services").empty());
}

TEST(json_services_are_sorted_and_carry_every_field) {
  const nlohmann::json doc = berth::services_json(
      {record("berthpanel-zeta", 2), record("berthpanel-alpha", 1)});
  const nlohmann::json& services = doc.at("services");
  CHECK(services.size() == 2);
  CHECK(services.at(0).at("service_name") == "berthpanel-alpha");
  CHECK(services.at(1).at("service_name") == "berthpanel-zeta");
  CHECK(services.at(0).at("port").get<std::uint16_t>() == 1);
  CHECK(services.at(0).at("repo") == "owner/berthpanel-alpha");
  CHECK(services.at(0).at("commit").get<std::string>().size() == 40);
}

TEST(json_text_is_pure_parseable_json) {
  const nlohmann::json doc = berth::services_json({record("berthpanel-a", 9)});
  const std::string text = berth::json_text(doc);
  CHECK(!text.empty());
  CHECK(text.back() == '\n');
  const nlohmann::json reparsed = nlohmann::json::parse(text);
  CHECK(reparsed == doc);
}

TEST(json_catalog_document_lists_recipes) {
  const nlohmann::json doc = berth::catalog_json();
  CHECK(doc.at("schema").get<int>() == berth::kJsonSchema);
  CHECK(doc.at("command").get<std::string>() == "catalog");
  CHECK(doc.at("recipes").is_array());
  CHECK(!doc.at("recipes").empty());
  CHECK(doc.at("recipes").at(0).contains("service_name"));
  CHECK(doc.at("recipes").at(0).contains("res"));
}

TEST(json_user_recipes_empty_is_an_empty_array) {
  const nlohmann::json doc = berth::user_recipes_json({});
  CHECK(doc.at("schema").get<int>() == berth::kJsonSchema);
  CHECK(doc.at("command").get<std::string>() == "recipe-list");
  CHECK(doc.at("recipes").is_array());
  CHECK(doc.at("recipes").empty());
}

TEST(json_human_renderers_are_not_json) {
  // The regression guard: the default path still emits text, never JSON.
  const std::string catalog = berth::render_catalog_list();
  CHECK(!catalog.empty());
  CHECK(catalog.front() != '{');
  const std::string empty_users = berth::render_user_recipes_list({});
  CHECK(empty_users == "no user recipes stored\n");
}
