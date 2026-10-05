#include "berth/catalog.h"

#include <algorithm>
#include <set>
#include <string>

#include "berth/httpclient.h"
#include "berth/recipes.h"
#include "berth/validate.h"
#include "netlog.h"
#include "runner.h"
#include "state.h"

using berth::builtin_catalog;
using berth::Recipe;
using berth::validate_recipe;

TEST(catalog_every_recipe_passes_the_validator) {
  const auto catalog = builtin_catalog();
  CHECK(catalog.is_ok());
  for (const Recipe& recipe : catalog.value()) {
    const auto valid = validate_recipe(recipe);
    CHECK(valid.is_ok());
  }
}

TEST(catalog_service_names_are_unique_and_prefixed) {
  const auto catalog = builtin_catalog();
  CHECK(catalog.is_ok());
  std::set<std::string> seen;
  for (const Recipe& recipe : catalog.value()) {
    CHECK(berth::validate_unit_name(recipe.service_name));
    CHECK(seen.insert(recipe.service_name).second);
  }
}

TEST(catalog_contract_every_service_has_resource_limits) {
  const auto catalog = builtin_catalog();
  for (const Recipe& recipe : catalog.value()) {
    CHECK(!recipe.res.memory_max.empty());
    CHECK(recipe.res.tasks_max > 0);
  }
}

TEST(catalog_contract_tools_cover_build_steps) {
  const auto catalog = builtin_catalog();
  for (const Recipe& recipe : catalog.value()) {
    for (const auto& step : recipe.build_steps) {
      CHECK(berth::tool_covers_step(recipe, step));
    }
  }
}

TEST(catalog_contract_every_service_binds_loopback_only) {
  const auto catalog = builtin_catalog();
  for (const Recipe& recipe : catalog.value()) {
    CHECK(recipe.start_cmd.find("127.0.0.1") != std::string::npos ||
          recipe.start_cmd.find("localhost") != std::string::npos);
    CHECK(recipe.start_cmd.find("0.0.0.0") == std::string::npos);
  }
}

TEST(catalog_commits_are_pinned_shas) {
  const auto catalog = builtin_catalog();
  for (const Recipe& recipe : catalog.value()) {
    CHECK(berth::validate_commit_sha(recipe.commit).is_ok());
  }
}

TEST(catalog_content_hosts_are_allowlisted) {
  const auto catalog = builtin_catalog();
  for (const Recipe& recipe : catalog.value()) {
    if (recipe.prebuilt.has_value()) {
      const auto url = berth::parse_http_url(recipe.prebuilt->url);
      CHECK(url.is_ok());
      CHECK(berth::classify_endpoint(url.value().host) ==
            berth::EndpointClass::kAllowed);
      CHECK(recipe.prebuilt->sha256.size() == 64);
    }
    for (const auto& pin : recipe.plugins) {
      const auto url = berth::parse_http_url(pin.url);
      CHECK(url.is_ok());
      CHECK(berth::classify_endpoint(url.value().host) ==
            berth::EndpointClass::kAllowed);
    }
  }
}

TEST(catalog_lookup_by_service_name) {
  CHECK(berth::builtin_recipe("berthpanel-memos").is_ok());
  CHECK(berth::builtin_recipe("berthpanel-nope").is_err());
}