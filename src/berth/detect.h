#pragma once

// src/berth/detect.h — stack detection → recipe DRAFT (plan stage 4.20).
//
// Detection NEVER deploys and NEVER runs repository code (§6.5: prefetch
// downloads, detection does not even do that — it only reads files). It
// materializes a draft recipe from what the checkout LOOKS like:
//   go.mod                  → go build, prefetch `go mod download`
//   package.json (+lockfile) → bun or pnpm, prefetch `install --ignore-scripts`
//   package-lock.json       → Err: npm is not in the v1 toolchain, and a
//                             silent bun guess would build the wrong tree
//   pyproject/requirements  → venv + pip
//   Dockerfile              → EXPOSE port and CMD flags are HINTS only;
//                             a Dockerfile alone never picks the stack
// Several stack families present at once is a loud Err ("ambiguous stack"),
// never a silent priority pick — the user must disambiguate, not the
// heuristic. The draft is heuristics, not truth: it ALWAYS passes
// validate_recipe (the detector heals its own tools coverage), and every
// warning names what a human must verify before the deploy.

#include <filesystem>
#include <string>
#include <vector>

#include "../util/result.h"
#include "recipes.h"

namespace berth {

struct DetectRequest {
  std::filesystem::path dir;  // a checked-out source tree
  std::string owner;          // recipe coordinates, validated like a deploy
  std::string name;
  std::string commit;  // pinned 40-hex SHA
};

struct RecipeDraft {
  Recipe recipe;  // valid by construction: validate_recipe() has passed
  std::vector<std::string> warnings;  // what a human must verify
};

[[nodiscard]] Result<RecipeDraft> detect_recipe(const DetectRequest& request);

}  // namespace berth
