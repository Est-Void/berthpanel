// Tests for lock.h (stage 4.3): acquisition, mutual exclusion, release
// on scope exit, and the SIGKILL scenario — the kernel drops the flock
// even when the holder never got to clean up, which is what makes the
// cleanup timer's "journal present AND lock free" reasoning sound (§6.7).

#include "berth/lock.h"

#include <unistd.h>

#include <climits>
#include <filesystem>
#include <string>

#include "runner.h"
#include "test_env.h"
#include "util/subprocess.h"

using berth::DeployLock;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.deploy_lock = p.state_dir / "deploy.lock";
  return p;
}

std::string self_exe() {
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return "";
  buf[n] = '\0';
  return std::string(buf);
}

}  // namespace

TEST(lock_acquire_and_release) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  {
    const auto lock = DeployLock::acquire(p);
    CHECK(lock.is_ok());
    CHECK(lock.value().held());
  }  // scope exit releases

  const auto again = DeployLock::acquire(p);
  CHECK(again.is_ok());
  CHECK(again.value().held());
}

TEST(lock_second_holder_is_refused_loudly) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto first = DeployLock::acquire(p);
  CHECK(first.is_ok());

  const auto second = DeployLock::acquire(p);
  CHECK(second.is_err());
  CHECK(second.error().ctx().find("already running") != std::string::npos);
}

TEST(lock_is_released_after_sigkill) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const std::string self = self_exe();
  CHECK(!self.empty());

  // Child takes the lock and pauses; the subprocess runner SIGKILLs it on
  // timeout and reaps it — no cleanup code runs in the child.
  const berth::Result<berth::RunOutput> killed =
      berth::run({self, "__test-lock-child", p.deploy_lock.string()},
                 berth::RunLimits{.timeout = std::chrono::milliseconds(400)});
  CHECK(killed.is_err());  // timed out => killed

  const auto lock = DeployLock::acquire(p);
  CHECK(lock.is_ok());
  CHECK(lock.value().held());
}
