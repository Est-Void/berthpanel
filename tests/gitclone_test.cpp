#include "berth/gitclone.h"

#include <string>
#include <vector>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"
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
