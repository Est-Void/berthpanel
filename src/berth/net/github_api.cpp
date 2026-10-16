#include "github_api.h"

#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../validate.h"
#include "client.h"
#include "fetch.h"
#include "tar.h"

namespace berth {

namespace {

constexpr std::string_view kCodeloadHost = "codeload.github.com";

// §6.6: only a pinned SHA and a well-formed owner/repo reach the network.
Result<void> check_pinned(const std::string& owner_repo,
                          const std::string& sha) {
  const Result<void> repo = validate_owner_repo(owner_repo);
  if (repo.is_err())
    return std::move(repo).error();
  return validate_commit_sha(sha);
}

HttpGetOptions github_options(const char* purpose, const std::string* token,
                              std::atomic<bool>* cancel) {
  HttpGetOptions options;
  options.kind = RequestKind::kApi;
  options.purpose = purpose;
  options.bearer_token = token;
  options.cancel = cancel;
  return options;
}

Result<HttpReply> get_ok(const std::string& url, const HttpGetOptions& options,
                         const Paths& paths, const std::string& what,
                         const std::string& owner_repo,
                         const std::string& sha) {
  Result<HttpReply> reply = http_get(url, options, paths);
  if (reply.is_err())
    return std::move(reply).error();
  if (reply.value().status != 200) {
    return Error("github_api: " + what + " answered status " +
                 std::to_string(reply.value().status) + " for " + owner_repo +
                 "@" + sha);
  }
  return reply;
}

// Exactly one top-level directory: guessing which one is not allowed here.
Result<std::filesystem::path> single_top_dir(
    const std::filesystem::path& dest_dir) {
  std::vector<std::filesystem::path> top_dirs;
  for (const auto& item : std::filesystem::directory_iterator(dest_dir)) {
    if (item.is_directory())
      top_dirs.push_back(item.path());
  }
  if (top_dirs.size() != 1) {
    return Error("github_api: archive has " + std::to_string(top_dirs.size()) +
                 " top-level directories in " + dest_dir.string() +
                 ", expected exactly 1");
  }
  return top_dirs.front();
}

}
Result<std::vector<TreeFile>> parse_tree_response(const std::string_view body) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& e) {
    return Error("trees api: parse: " + std::string(e.what()));
  }
  if (!doc.is_object() || !doc.contains("tree") || !doc["tree"].is_array()) {
    return Error("trees api: no tree array");
  }
  // Every present field is type-checked: get<T>() throws type_error on a
  // wrong-typed field and only parse_error is caught, so it aborts (§6.10).
  if (doc.contains("truncated") && !doc["truncated"].is_boolean()) {
    return Error("trees api: 'truncated' is not a boolean");
  }
  if (doc.value("truncated", false)) {
    return Error(
        "trees api: listing truncated by GitHub; refusing incomplete data");
  }

  std::vector<TreeFile> out;
  for (const nlohmann::json& item : doc["tree"]) {
    // A row that is not a blob with a string path is skipped.
    if (!item.is_object() || !item.contains("path") ||
        !item["path"].is_string() || !item.contains("type") ||
        !item["type"].is_string() ||
        item["type"].get<std::string>() != "blob") {
      continue;
    }
    TreeFile file;
    file.path = item["path"].get<std::string>();
    if (item.contains("sha") && item["sha"].is_string())
      file.sha = item["sha"].get<std::string>();
    if (item.contains("size") && item["size"].is_number_unsigned())
      file.size = item["size"].get<std::uint64_t>();
    out.push_back(std::move(file));
  }
  return out;
}

Result<std::vector<TreeFile>> fetch_tree(const std::string& owner_repo,
                                         const std::string& sha,
                                         const Paths& paths,
                                         const std::string* token,
                                         std::atomic<bool>* cancel) {
  const Result<void> valid = check_pinned(owner_repo, sha);
  if (valid.is_err())
    return std::move(valid).error();

  const std::string url = std::string("https://api.github.com/repos/") +
                          owner_repo + "/git/trees/" + sha + "?recursive=1";
  Result<HttpReply> reply =
      get_ok(url, github_options("github-trees", token, cancel), paths,
             "trees api", owner_repo, sha);
  if (reply.is_err())
    return std::move(reply).error();
  return parse_tree_response(reply.value().body);
}

Result<std::string> parse_default_branch(const std::string_view body) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& e) {
    return Error("repos api: parse: " + std::string(e.what()));
  }
  if (!doc.is_object() || !doc.contains("default_branch") ||
      !doc["default_branch"].is_string() ||
      doc["default_branch"].get<std::string>().empty()) {
    return Error("repos api: no default_branch in the response");
  }
  return doc["default_branch"].get<std::string>();
}

Result<std::string> parse_commit_sha(const std::string_view body) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& e) {
    return Error("commits api: parse: " + std::string(e.what()));
  }
  if (!doc.is_object() || !doc.contains("sha") || !doc["sha"].is_string()) {
    return Error("commits api: no sha in the response");
  }
  const std::string sha = doc["sha"].get<std::string>();
  const Result<void> valid = validate_commit_sha(sha);
  if (valid.is_err())
    return std::move(valid).error();
  return sha;
}

Result<std::string> resolve_head_sha(const Paths& paths,
                                     const std::string& owner_repo,
                                     const std::string& ref,
                                     std::atomic<bool>* cancel) {
  const Result<void> repo = validate_owner_repo(owner_repo);
  if (repo.is_err())
    return std::move(repo).error();

  const auto repo_url = "https://api.github.com/repos/" + owner_repo;
  // Token never attached: the wizard resolves public repos only (§6.3).
  Result<HttpReply> reply =
      get_ok(repo_url, github_options("github-repos", nullptr, cancel), paths,
             "repos api", owner_repo, "default branch");
  if (reply.is_err())
    return std::move(reply).error();
  Result<std::string> branch = parse_default_branch(reply.value().body);
  if (branch.is_err())
    return std::move(branch).error();

  const std::string target = ref.empty() ? branch.value() : ref;
  Result<HttpReply> head =
      get_ok(repo_url + "/commits/" + target,
             github_options("github-commits", nullptr, cancel), paths,
             "commits api", owner_repo, "ref " + target);
  if (head.is_err())
    return std::move(head).error();
  return parse_commit_sha(head.value().body);
}

Result<std::filesystem::path> fetch_and_extract(
    const std::string& owner_repo, const std::string& sha,
    const std::filesystem::path& dest_dir, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel) {
  const Result<void> valid = check_pinned(owner_repo, sha);
  if (valid.is_err())
    return std::move(valid).error();

  const std::string url = std::string("https://") + std::string(kCodeloadHost) +
                          "/" + owner_repo + "/tar.gz/" + sha;
  Result<HttpReply> reply =
      get_ok(url, github_options("github-archive", token, cancel), paths,
             "codeload", owner_repo, sha);
  if (reply.is_err())
    return std::move(reply).error();

  const Result<std::size_t> extracted =
      tar_extract_gz(reply.value().body, dest_dir);
  if (extracted.is_err())
    return std::move(extracted).error();
  return single_top_dir(dest_dir);
}

}
