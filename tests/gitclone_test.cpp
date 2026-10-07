#include "berth/gitclone.h"

#include <string>
#include <vector>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"
#include "util/subprocess.h"
#include "util/which.h"

using berth::clone_pinned;
using berth::Result;
using berth::token_allowed_for_url;

namespace {

Result<std::string> git(const std::vector<std::string>& args,
                        const std::chrono::milliseconds timeout =
                            std::chrono::milliseconds(60000)) {
  const Result<std::string> binary = berth::resolve_executable("git");
  if (binary.is_err())
    return std::move(binary).error();
  std::vector<std::string> argv{binary.value()};
  argv.insert(argv.end(), args.begin(), args.end());
  berth::RunLimits limits;
  limits.timeout = timeout;
  const berth::Result<berth::RunOutput> out = berth::run(argv, limits);
  if (out.is_err())
    return std::move(out).error();
  if (out.value().exit_code != 0) {
    return berth::Error("git: exit " + std::to_string(out.value().exit_code) +
                        ": " + out.value().err);
  }
  return out.value().out;
}

std::string trimmed(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) {
    s.pop_back();
  }
  return s;
}

Result<std::string> make_local_repo(const std::filesystem::path& dir) {
  CHECK(berth::ensure_dir(dir).is_ok());
  CHECK(git({"init", "--quiet", "-b", "main", dir.string()}).is_ok());
  CHECK(git({"-C", dir.string(), "config", "user.email", "t@example.invalid"})
            .is_ok());
  CHECK(git({"-C", dir.string(), "config", "user.name", "test"}).is_ok());
  CHECK(git({"-C", dir.string(), "commit", "--quiet", "--allow-empty", "-m",
             "init"})
            .is_ok());
  return trimmed(git({"-C", dir.string(), "rev-parse", "HEAD"}).value());
}

}  // namespace

TEST(gitclone_token_requires_exact_host_match) {
  CHECK(token_allowed_for_url("https://github.com/o/r.git", "github.com"));
  CHECK(!token_allowed_for_url("https://github.com.evil.io/o/r.git",
                               "github.com"));
  CHECK(!token_allowed_for_url("https://evil.io/?x=github.com", "github.com"));
  CHECK(!token_allowed_for_url("https://evil.io/github.com/o/r", "github.com"));
  CHECK(!token_allowed_for_url("http://github.com/o/r.git", "github.com"));
  CHECK(!token_allowed_for_url("file:///tmp/repo", "github.com"));
  CHECK(token_allowed_for_url("https://api.github.com/x", "api.github.com"));
}

TEST(gitclone_clones_pinned_commit_from_local_repo) {
  const TempDir tmp;
  const auto source = tmp.path() / "source";
  const berth::Result<std::string> made = make_local_repo(source);
  CHECK(made.is_ok());
  const std::string sha = made.value();
  CHECK(sha.size() == 40);

  const auto dest = tmp.path() / "work" / "berthpanel-memos";
  const berth::Result<void> cloned = clone_pinned({
      .url = "file://" + source.string(),
      .commit = sha,
      .dest_dir = dest,
      .token = nullptr,
  });
  CHECK(cloned.is_ok());
  CHECK(std::filesystem::exists(dest / ".git"));
  const std::string head =
      trimmed(git({"-C", dest.string(), "rev-parse", "HEAD"}).value());
  CHECK(head == sha);
}

TEST(gitclone_refuses_nonempty_destination) {
  const TempDir tmp;
  const auto source = tmp.path() / "source";
  const berth::Result<std::string> made = make_local_repo(source);
  CHECK(made.is_ok());
  const auto dest = tmp.path() / "work";
  CHECK(berth::ensure_dir(dest).is_ok());
  CHECK(berth::atomic_write(dest / "keep.txt", "x").is_ok());

  const berth::Result<void> cloned = clone_pinned({
      .url = "file://" + source.string(),
      .commit = made.value(),
      .dest_dir = dest,
  });
  CHECK(cloned.is_err());
  CHECK(std::filesystem::exists(dest / "keep.txt"));
}

TEST(gitclone_refuses_token_for_foreign_host) {
  const TempDir tmp;
  const std::string token = "ghp_secret";
  const berth::Result<void> cloned = clone_pinned({
      .url = "https://github.com.evil.io/o/r.git",
      .commit = std::string(40, 'a'),
      .dest_dir = tmp.path() / "work",
      .token = &token,
  });
  CHECK(cloned.is_err());
  CHECK(!std::filesystem::exists(tmp.path() / "work"));
}

TEST(gitclone_refuses_branch_instead_of_sha) {
  const TempDir tmp;
  const berth::Result<void> cloned = clone_pinned({
      .url = "file:///tmp/whatever",
      .commit = "main",
      .dest_dir = tmp.path() / "work",
  });
  CHECK(cloned.is_err());
  CHECK(cloned.error().ctx().find("40-char") != std::string::npos);
}

TEST(resolve_ref_sha_resolves_head_branch_and_peels_tags) {
  const TempDir tmp;
  const auto source = tmp.path() / "source";
  const berth::Result<std::string> made = make_local_repo(source);
  CHECK(made.is_ok());
  const std::string head_sha = made.value();
  CHECK(
      git({"-C", source.string(), "tag", "-a", "v1", "-m", "release"}).is_ok());
  const std::string tag_sha = trimmed(
      git({"-C", source.string(), "rev-list", "-n", "1", "v1^{}"}).value());
  CHECK(tag_sha == head_sha);

  const std::string url = "file://" + source.string();
  const auto short_timeout = std::chrono::milliseconds(60000);

  CHECK(berth::resolve_ref_sha(url, "HEAD", short_timeout, nullptr).value() ==
        head_sha);
  CHECK(berth::resolve_ref_sha(url, "", short_timeout, nullptr).value() ==
        head_sha);
  CHECK(berth::resolve_ref_sha(url, "main", short_timeout, nullptr).value() ==
        head_sha);
  // An annotated tag resolves to the peeled commit, not the tag object.
  CHECK(berth::resolve_ref_sha(url, "v1", short_timeout, nullptr).value() ==
        tag_sha);

  // A named ref never falls back to HEAD: that would deploy a moved branch.
  CHECK(berth::resolve_ref_sha(url, "no-such-ref", short_timeout, nullptr)
            .is_err());
  // Option-shaped and whitespace-bearing refs are refused, never escaped.
  CHECK(
      berth::resolve_ref_sha(url, "--upload-pack=evil", short_timeout, nullptr)
          .is_err());
  CHECK(berth::resolve_ref_sha(url, "main HEAD", short_timeout, nullptr)
            .is_err());
  CHECK(berth::resolve_ref_sha(url, "a..b", short_timeout, nullptr).is_err());
}

TEST(github_owner_repo_from_url_accepts_only_https_github_com) {
  CHECK(berth::github_owner_repo_from_url("https://github.com/usmarc/go-memos")
            .value() == "usmarc/go-memos");
  CHECK(
      berth::github_owner_repo_from_url("https://github.com/o/r.git").value() ==
      "o/r");
  CHECK(berth::github_owner_repo_from_url("https://github.com/o/r/tree/main")
            .is_err());

  // §6.1 front door: exact host match, https only.
  CHECK(berth::github_owner_repo_from_url("https://github.com.evil.io/o/r")
            .is_err());
  CHECK(berth::github_owner_repo_from_url("https://evil.io/o/r?x=github.com")
            .is_err());
  CHECK(berth::github_owner_repo_from_url("http://github.com/o/r").is_err());
  CHECK(berth::github_owner_repo_from_url("file:///tmp/repo").is_err());
  CHECK(berth::github_owner_repo_from_url("ssh://git@github.com/o/r").is_err());
}

TEST(clone_pinned_github_validates_before_any_network_call) {
  const TempDir tmp;
  const auto dest = tmp.path() / "work";

  CHECK(berth::clone_pinned_github("not-a-repo", std::string(40, 'a'), dest,
                                   nullptr, std::chrono::milliseconds(1000),
                                   nullptr)
            .is_err());
  CHECK(berth::clone_pinned_github("o/r", "main", dest, nullptr,
                                   std::chrono::milliseconds(1000), nullptr)
            .is_err());
  CHECK(berth::clone_pinned_github("o/r", std::string(40, 'A'), dest, nullptr,
                                   std::chrono::milliseconds(1000), nullptr)
            .is_err());
  CHECK(!std::filesystem::exists(dest));
}
