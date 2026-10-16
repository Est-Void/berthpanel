#include "berth/sandbox.h"

#include <mutex>
#include <string>

#include "core/flags.h"
#include "runner.h"
#include "test_env.h"

using berth::sandbox_argv_body;
using berth::SandboxSpec;
using berth::verify_sandbox;

namespace {

bool body_has(const std::vector<std::string>& body, const std::string& needle) {
  for (const std::string& element : body) {
    if (element == needle)
      return true;
  }
  return false;
}

std::size_t count_property(const std::vector<std::string>& body,
                           const std::string& key) {
  std::size_t count = 0;
  for (const std::string& element : body) {
    if (element.rfind("--property=" + key, 0) == 0)
      ++count;
  }
  return count;
}

}
TEST(sandbox_argv_always_carries_the_full_grade_set) {
  const std::vector<std::string> body =
      sandbox_argv_body({.argv = {"/bin/true"},
                         .working_dir = {},
                         .env_extra = {},
                         .unit_name = {},
                         .memory_max_mb = 2048,
                         .tasks_max = 512});
  for (const char* property :
       {"--property=MemoryMax=2048M", "--property=TasksMax=512",
        "--property=NoNewPrivileges=yes", "--property=PrivateTmp=yes",
        "--property=ProtectSystem=strict",
        "--property=ProtectKernelTunables=yes",
        "--property=RestrictSUIDSGID=yes", "--property=LockPersonality=yes",
        "--property=CapabilityBoundingSet=",
        "--property=RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6"}) {
    CHECK(body_has(body, property));
  }
  CHECK(body_has(body, "--user"));
  CHECK(body_has(body, "--wait"));
  CHECK(body_has(body, "--"));
}

TEST(sandbox_argv_keeps_paths_with_spaces_as_one_element) {
  const std::vector<std::string> body =
      sandbox_argv_body({.argv = {"/bin/sh", "-c", "echo hi"},
                         .working_dir = "/home/u/My Apps/berthpanel-memos",
                         .env_extra = {},
                         .unit_name = {},
                         .memory_max_mb = 2048,
                         .tasks_max = 512});
  CHECK(body_has(body, "--working-directory=/home/u/My Apps/berthpanel-memos"));
  // ReadWritePaths is a list: the space is escaped so systemd-run does not
  // split the path.
  CHECK(body_has(
      body, "--property=ReadWritePaths=/home/u/My\\ Apps/berthpanel-memos"));
}

TEST(sandbox_argv_has_no_way_to_disable_it) {
  const std::vector<std::string> body =
      sandbox_argv_body({.argv = {"/bin/true"},
                         .working_dir = {},
                         .env_extra = {},
                         .unit_name = {},
                         .memory_max_mb = 2048,
                         .tasks_max = 512});
  for (const std::string& element : body) {
    const std::string lowered = element;
    CHECK(lowered.find("disable") == std::string::npos);
    CHECK(lowered.find("no-sandbox") == std::string::npos);
    CHECK(lowered.find("Property=PrivateNetwork=no") == std::string::npos);
  }
  // Every property is a --property= element; nothing can unset them from
  // outside because the caller only supplies argv/env/limits.
  CHECK(count_property(body, "MemoryMax=") == 1);
}

TEST(sandbox_argv_names_the_transient_unit_when_given_one) {
  // A named unit survives its client: the orphan-termination design
  // (stage 4.17) depends on this flag.
  const std::vector<std::string> body =
      sandbox_argv_body({.argv = {"/bin/true"},
                         .working_dir = {},
                         .env_extra = {},
                         .unit_name = "berthpanel-build-berthpanel-memos",
                         .memory_max_mb = 2048,
                         .tasks_max = 512});
  CHECK(body_has(body, "--unit=berthpanel-build-berthpanel-memos"));
}

TEST(sandbox_live_verification_opt_in) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  if (!(enabled.is_ok() && enabled.value().value_or(false))) {
    return;
  }
  const auto audit = verify_sandbox();
  CHECK(audit.is_ok());
  CHECK(audit.value().full_grade());
}
