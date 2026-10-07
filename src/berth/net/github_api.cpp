// src/berth/net/github_api.cpp — see github_api.h for the contract.

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

// §6.6: a branch name here would deploy whatever HEAD points at now. Only a
// pinned SHA and a well-formed owner/repo reach the network.
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

// A GitHub tarball carries exactly one top-level directory (owner-repo-sha).
// More or fewer means the layout assumption is broken, and picking one
// arbitrarily is exactly the kind of guess that must not happen here.
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

}  // namespace

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
  if (doc.value("truncated", false)) {
    return Error(
        "trees api: listing truncated by GitHub; refusing incomplete data");
  }

  std::vector<TreeFile> out;
  for (const nlohmann::json& item : doc["tree"]) {
    // trees/commits are not files, and a row without a path is not ours.
    if (!item.is_object() || !item.contains("path") ||
        item.value("type", "") != "blob") {
      continue;
    }
    TreeFile file;
    file.path = item["path"].get<std::string>();
    file.sha = item.value("sha", std::string());
    file.size = item.value("size", std::uint64_t{0});
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

}  // namespace berth
