#include "wizard.h"

#include <optional>
#include <string>
#include <utility>

#include "../core/names.h"
#include "../core/netlog.h"
#include "catalog.h"
#include "detect.h"
#include "gitclone.h"
#include "net/github_api.h"
#include "userrecipes.h"
#include "util/tmpdir.h"
#include "validate.h"

namespace berth {

namespace {

void say(const DraftProgress& progress, const std::string& line) {
  if (progress) {
    progress(line);
  }
}

}
Result<DraftResult> prepare_draft(const Paths& paths,
                                  const DraftRequest& request,
                                  std::atomic<bool>* cancel,
                                  const DraftProgress& progress) {
  Result<std::string> owner_repo = github_owner_repo_from_url(request.url);
  if (owner_repo.is_err()) {
    return std::move(owner_repo).error();
  }
  const std::size_t slash = owner_repo.value().find('/');
  if (slash == std::string::npos) {
    return Error("wizard: '" + owner_repo.value() +
                 "' is not an owner/repo pair");
  }
  const std::string owner = owner_repo.value().substr(0, slash);
  const std::string name = owner_repo.value().substr(slash + 1);

  // Resolved NOW: a draft freezes one commit, never a branch (§6.6). No token.
  std::string commit(request.ref);
  if (commit.empty() || validate_commit_sha(commit).is_err()) {
    say(progress, "resolving " + owner_repo.value() + "@" +
                      (request.ref.empty() ? "default branch" : request.ref) +
                      " via api...");
    Result<std::string> resolved =
        resolve_head_sha(paths, owner_repo.value(), request.ref, cancel);
    if (resolved.is_err()) {
      return std::move(resolved).error();
    }
    commit = resolved.value();
  }

  // NESTED: clone_pinned chmods the parent, which must stay the TempDir.
  Result<TempDir> scratch = TempDir::create("draft");
  if (scratch.is_err()) {
    return std::move(scratch).error();
  }
  const std::filesystem::path dir = scratch.value().path() / "repo";

  say(progress,
      "cloning " + owner_repo.value() + "@" + commit.substr(0, 7) + "...");

  // §6.1: a clone does NOT go through httpclient, so log it BEFORE git runs.
  const Result<void> logged = netlog_append(
      paths,
      NetlogEntry{.host = kTokenHost,
                  .ip = "",
                  .port = 443,
                  .purpose = "git-fetch",
                  .note = "draft clone " + owner_repo.value() + "@" + commit,
                  .allowed = true});
  if (logged.is_err()) {
    return std::move(logged).error();
  }

  const Result<void> cloned = clone_pinned({
      .url = "https://" + std::string(kTokenHost) + "/" + owner_repo.value() +
             ".git",
      .commit = commit,
      .dest_dir = dir,
      .timeout = std::chrono::milliseconds(600000),
      .cancel = cancel,
  });
  if (cloned.is_err()) {
    return std::move(cloned).error();
  }

  const DetectRequest detected{
      .dir = dir, .owner = owner, .name = name, .commit = commit};
  Result<RecipeDraft> draft = detect_recipe(detected);
  if (draft.is_err()) {
    return std::move(draft).error();
  }

  // A missing tool is a WARNING: install it; preflight gates the deploy.
  Result<std::vector<ToolStatus>> tools =
      check_tools(draft.value().recipe.tools, cancel);
  if (tools.is_err()) {
    return std::move(tools).error();
  }

  DraftResult result;
  RecipeDraft prepared = std::move(draft).value();
  result.draft = std::move(prepared.recipe);
  result.warnings = std::move(prepared.warnings);
  result.owner_repo = owner_repo.value();
  result.commit = commit;
  result.tools = std::move(tools).value();
  return result;
}

Result<DraftClash> find_recipe_clash(const Paths& paths,
                                     const std::string_view service_name) {
  // Same order and sources as user_recipe_store's policy; the two must agree.
  Result<std::optional<Recipe>> stored = user_recipe_get(paths, service_name);
  if (stored.is_err()) {
    return std::move(stored).error();
  }
  if (stored.value().has_value()) {
    DraftClash clash;
    clash.exists = true;
    clash.service_name = std::string(service_name);
    clash.existing_commit = stored.value()->commit;
    clash.existing_path = paths.recipes_dir / (clash.service_name + ".json");
    return clash;
  }
  Result<Recipe> builtin = builtin_recipe(service_name);
  if (builtin.is_err()) {
    return DraftClash{};  // no builtin entry: the name is free
  }
  DraftClash clash;
  clash.exists = true;
  clash.in_catalog = true;
  clash.service_name = std::string(service_name);
  clash.existing_commit = builtin.value().commit;
  return clash;
}

Result<std::filesystem::path> store_draft(const Paths& paths,
                                          const Recipe& recipe,
                                          const bool force) {
  return user_recipe_store(paths, recipe, force);
}

std::string render_draft_report(const DraftResult& result,
                                const DraftClash& clash) {
  const Recipe& draft = result.draft;
  std::string out = "draft " + draft.service_name + " (" + draft.language +
                    ", " + draft.res.memory_max + "/" +
                    std::to_string(draft.res.tasks_max) + ")\n" +
                    "  repo:  " + result.owner_repo + " @ " + result.commit +
                    "\n  start: " + draft.start_cmd + "\n  tools:\n" +
                    render_tool_report(result.tools);
  for (const std::string& warning : result.warnings) {
    out += "  warning: " + warning + "\n";
  }
  if (clash.exists) {
    // The question comes BEFORE the store: it says what a store WOULD replace.
    out += "  a recipe for " + clash.service_name + " already exists" +
           (clash.in_catalog ? " in the builtin catalog" : " (user recipe)") +
           "";
    if (!clash.existing_commit.empty()) {
      out += ", pinned " + clash.existing_commit.substr(0, 7);
    }
    out += " — storing needs an explicit overwrite\n";
  }
  return out;
}

}
