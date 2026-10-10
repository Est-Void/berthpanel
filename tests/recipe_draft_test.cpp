// Tests for the 4.21 wizard pin: the GitHub API parsers are pure and run
// offline on fixtures; resolve_head_sha is the only networked part and is
// strictly opt-in via BERTHPANEL_RUN_NETWORK_TESTS (never in CI, §9).
// The URL front door itself is gitclone_test.cpp's contract — not
// duplicated here.

#include "berth/net/github_api.h"

#include <mutex>
#include <string>

#include "core/flags.h"
#include "core/paths.h"
#include "runner.h"
#include "test_env.h"
#include "util/env.h"

namespace {

const char* kRepoReply =
    R"({"id":1,"name":"http-echo","full_name":"hashicorp/http-echo",)"
    R"("default_branch":"main","private":false})";

const char* kCommitReply =
    R"({"sha":"2b2192d4e153bd04f1d325b60fd880cf00d68b01",)"
    R"("commit":{"message":"initial"}})";

}  // namespace

TEST(parse_default_branch_reads_the_repos_reply) {
  const auto branch = berth::parse_default_branch(kRepoReply);
  CHECK(branch.is_ok());
  CHECK(branch.value() == "main");
}

TEST(parse_default_branch_refuses_a_reply_without_one) {
  CHECK(berth::parse_default_branch("{}").is_err());
  CHECK(berth::parse_default_branch(R"({"default_branch":""})").is_err());
  CHECK(berth::parse_default_branch("[1,2]").is_err());
  CHECK(berth::parse_default_branch("not json").is_err());
}

TEST(parse_commit_sha_reads_the_commits_reply) {
  const auto sha = berth::parse_commit_sha(kCommitReply);
  CHECK(sha.is_ok());
  CHECK(sha.value() == "2b2192d4e153bd04f1d325b60fd880cf00d68b01");
}

TEST(parse_commit_sha_refuses_a_branch_or_garbage) {
  // §6.6: a branch name here would deploy whatever HEAD points at now.
  CHECK(berth::parse_commit_sha(R"({"sha":"main"})").is_err());
  CHECK(berth::parse_commit_sha(R"({"commit":{"sha":"abc"}})").is_err());
  CHECK(berth::parse_commit_sha("[]").is_err());
  CHECK(berth::parse_commit_sha("").is_err());
}

TEST(resolve_head_sha_resolves_the_pinned_head_opt_in) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_NETWORK_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_NETWORK_TESTS");
  if (!(enabled.is_ok() && enabled.value().value_or(false))) {
    return;
  }
  const TempDir tmp;
  berth::Paths paths;
  paths.net_log = tmp.path() / "net.log";

  const auto sha =
      berth::resolve_head_sha(paths, "hashicorp/http-echo", "", nullptr);
  CHECK(sha.is_ok());
  CHECK(sha.value().size() == 40);

  // A 404 from the API is data, not a transport outage: an unknown repo is
  // refused with its own message, never misreported as "network down".
  const auto missing =
      berth::resolve_head_sha(paths, "hashicorp/no-such-repo-4x", "", nullptr);
  CHECK(missing.is_err());
}
