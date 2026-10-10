#pragma once

// src/berth/wizard.h — "paste a GitHub URL, get a draft recipe" (plan stage
// 4.21, dev.md §8). The engine half of the wizard; the CLI and the panel are
// two front ends over ONE flow, because two flows would drift (§11: the old
// code had a deploy path in the TUI and another in the CLI).
//
// EVERYTHING here blocks: an API round trip, a scratch clone, a directory
// walk, a subprocess per tool. It is berth/, so the TUI may only call it on a
// worker (dev.md §5).
//
// Split in two on purpose:
//   prepare_draft()   URL → front-door policy → resolve the pin → scratch
//                     clone → detect → tool check. Expensive, and it STORES
//                     NOTHING.
//   store_draft()     the draft to disk, through user_recipe_store() — the one
//                     store, with the one duplicate policy (§8: overwriting a
//                     user recipe or shadowing a builtin one is an explicit
//                     act).
//
// The split is why the wizard does not simply save what it detected: whether
// to store is a HUMAN decision (the CLI answers it with --force, the panel
// with a confirmation), and a wizard that stored before asking could not ask.
// Reproducibility is still intact: nothing is ever deployed that is not on
// disk, because the deploy always comes after the store.

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
  std::string url;  // https, host exactly github.com (dev.md §6.1/§12.9)
  std::string ref;  // empty = the default branch; a 40-hex SHA is taken as
                    // given, anything else is resolved through api.github.com
};

// Everything detection learned, plus what the tools check says. Nothing here
// has been written to disk yet.
struct DraftResult {
  Recipe draft;  // valid by construction: detect_recipe heals its own coverage
  std::string owner_repo;
  std::string commit;  // the resolved pin, 40-hex — the draft NEVER tracks a
                       // branch (§6.6)
  std::vector<ToolStatus> tools;
  std::vector<std::string> warnings;
};

// What a name collision would replace. Found BEFORE storing so the caller can
// ask instead of clobbering, and so the question can be asked with the OLD
// pin in it.
struct DraftClash {
  bool exists = false;
  bool in_catalog = false;  // a builtin entry rather than a stored file
  std::string service_name;
  std::string existing_commit;  // empty when the existing recipe is unpinned
  std::filesystem::path existing_path;
};

// Progress lines for a screen or a terminal. Called from the worker, so a
// caller that writes into the panel must take its own lock.
using DraftProgress = std::function<void(const std::string&)>;

// The whole expensive half. Err with context on every failure; the scratch
// checkout is removed however this ends (TempDir RAII), and the clone attempt
// is appended to net.log BEFORE git runs, so the line exists even if the
// process dies mid-clone (§6.1).
[[nodiscard]] Result<DraftResult> prepare_draft(
    const Paths& paths, const DraftRequest& request, std::atomic<bool>* cancel,
    const DraftProgress& progress = {});

// Is this service_name already taken, and by what? Ok(clash{exists:false}) is
// the common answer. A name that is neither a stored recipe nor a builtin one
// is the only one that may be written without an explicit act.
[[nodiscard]] Result<DraftClash> find_recipe_clash(
    const Paths& paths, std::string_view service_name);

// The draft to disk. `force` is the human's answer, never a default.
// Thin over user_recipe_store() on purpose: one store, one policy.
[[nodiscard]] Result<std::filesystem::path> store_draft(const Paths& paths,
                                                        const Recipe& recipe,
                                                        bool force);

// PURE: the report `__recipe-draft` prints. The panel renders the same facts
// through view_deploy.h's review lines, from the same DraftResult — one source
// of truth, two surfaces.
[[nodiscard]] std::string render_draft_report(const DraftResult& result,
                                              const DraftClash& clash);

}  // namespace berth