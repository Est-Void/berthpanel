#pragma once

// Detection only reads the checkout: it never fetches, never runs repo code.
#include <filesystem>
#include <string>
#include <vector>

#include "../util/result.h"
#include "recipes.h"

namespace berth {

struct DetectRequest {
  std::filesystem::path dir;
  std::string owner;
  std::string name;
  std::string commit;
};

struct RecipeDraft {
  Recipe recipe;  // valid by construction: validate_recipe() has passed
  std::vector<std::string> warnings;
};

[[nodiscard]] Result<RecipeDraft> detect_recipe(const DetectRequest& request);

}
