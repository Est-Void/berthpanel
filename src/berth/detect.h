#pragma once

// src/berth/detect.h — stack detection → recipe DRAFT (plan stage 4.20).
//
// Detection NEVER deploys and NEVER runs repository code (§6.5: prefetch
// downloads, detection does not even do that — it only reads files). It
// materializes a draft recipe from what the checkout LOOKS like:
//   go.mod                  → go build
//   package.json (+lockfile) → bun or pnpm (lockfile decides, bun default)
//   pyproject/requirements  → venv + pip
//   Dockerfile EXPOSE       → port HINT only
// The draft is heuristics, not truth: it always passes validate_recipe so
// quick-accept is honest, and every field is editable before it becomes a
// stored user recipe (stage 4.21). Priority when several stacks match:
// go > node > python.

#include <filesystem>
#include <string>

#include "../util/result.h"
#include "recipes.h"

namespace berth {

struct DetectRequest {
  std::filesystem::path dir;  // a checked-out source tree
  std::string owner;          // recipe coordinates, validated like a deploy
  std::string name;
  std::string commit;  // pinned 40-hex SHA
};

[[nodiscard]] Result<Recipe> detect_recipe(const DetectRequest& request);

}  // namespace berth
