#pragma once

// "paste a GitHub URL, get a draft recipe": the CLI and the panel are two front
// ends over ONE flow, because two flows would drift (§11). EVERYTHING here
// blocks, so the TUI may only call it on a worker.
// prepare_draft() resolves, clones, detects and checks tools, and STORES
// NOTHING; store_draft() writes through user_recipe_store(). Whether to store
// is a HUMAN decision: a wizard that stored before asking could not ask.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"
#include "recipes.h"
#include "toolcheck.h"

namespace berth {

struct DraftRequest {
  std::string url;  // https, host exactly github.com (§6.1)
  std::string ref;  // empty = default branch; a 40-hex SHA is taken as given
};

struct DraftResult {
  Recipe draft;  // valid by construction: detect_recipe heals its own coverage
  std::string owner_repo;
  std::string commit;  // the resolved 40-hex pin; a draft NEVER tracks a branch
  std::vector<ToolStatus> tools;
  std::vector<std::string> warnings;
};

// Found BEFORE storing, so the caller can ask instead of clobbering.
struct DraftClash {
  bool exists = false;
  bool in_catalog = false;  // a builtin entry rather than a stored file
  std::string service_name;
  std::string existing_commit;  // empty when the existing recipe is unpinned
  std::filesystem::path existing_path;
};

// Called from the worker: a caller writing into the panel takes its own lock.
using DraftProgress = std::function<void(const std::string&)>;

// The scratch goes however this ends (TempDir RAII); the clone attempt is
// net.log'd BEFORE git runs (§6.1).
[[nodiscard]] Result<DraftResult> prepare_draft(
    const Paths& paths, const DraftRequest& request, std::atomic<bool>* cancel,
    const DraftProgress& progress = {});

// Only a name that is neither stored nor builtin is writable without an act.
[[nodiscard]] Result<DraftClash> find_recipe_clash(
    const Paths& paths, std::string_view service_name);

// `force` is the human's answer, never a default.
[[nodiscard]] Result<std::filesystem::path> store_draft(const Paths& paths,
                                                        const Recipe& recipe,
                                                        bool force);

[[nodiscard]] std::string render_draft_report(const DraftResult& result,
                                              const DraftClash& clash);

}
