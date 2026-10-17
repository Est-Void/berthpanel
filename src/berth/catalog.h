#pragma once

#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "recipes.h"
#include "stacks.h"

namespace berth {

[[nodiscard]] Result<std::vector<Recipe>> builtin_catalog();

[[nodiscard]] Result<Recipe> builtin_recipe(std::string_view service_name);

[[nodiscard]] Result<Recipe> resolve_recipe(const Paths& paths,
                                            std::string_view service_name);

[[nodiscard]] std::string render_catalog_list();

[[nodiscard]] nlohmann::json catalog_json();

// Builtin multi-unit recipes (stage 9.3). The pilot is the only entry.
[[nodiscard]] Result<std::vector<Stack>> builtin_stacks();

[[nodiscard]] Result<Stack> builtin_stack(std::string_view service_name);

[[nodiscard]] std::string render_stack_list();

[[nodiscard]] nlohmann::json stacks_json();

}
