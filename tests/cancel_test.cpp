// The systemctl shim lives on PATH; every test below holds env_test_lock.

#include "berth/cancel.h"
#include "berth/units.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "core/flags.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/env.h"
#include "util/subprocess.h"
#include "util/which.h"

using berth::Result;
using berth::StopPolicy;
using berth::SystemdUnitTerminator;

namespace {

// Shim: is-active reads a state file, stop/kill write verdicts; log every call.
class FakeSystemctl {
 public:
  explicit FakeSystemctl(const std::filesystem::path& dir,
                         const std::string& initial,
                         const std::string& stop_writes,
                         const std::string& kill_writes)
      : state_(dir / "state"), log_(dir / "log") {
    write(state_, initial);
    const std::filesystem::path bin = dir / "bin";
    std::filesystem::create_directories(bin);
    std::string script = "#!/bin/sh\n";
    script += "STATE=" + state_.string() + "\n";
    script += "LOG=" + log_.string() + "\n";
    script += "printf '%s\\n' \"$*\" >> \"$LOG\"\n";
    // Skip the leading global flag to reach the subcommand.
    script += "cmd=$1\n";
    script += "case \"$cmd\" in --*) shift; cmd=$1 ;; esac\n";
    script += "shift\n";
    script += "case \"$cmd\" in\n";
    script += "  is-active)\n";
    script += "    state=$(cat \"$STATE\" 2>/dev/null || printf inactive)\n";
    script += "    printf '%s\\n' \"$state\"\n";
    script += "    if [ \"$state\" = \"active\" ]; then exit 0; fi\n";
    script += "    exit 3 ;;\n";
    script +=
        "  stop) printf '%s' \"" + stop_writes + "\" > \"$STATE\"; exit 0 ;;\n";
    script +=
        "  kill) printf '%s' \"" + kill_writes + "\" > \"$STATE\"; exit 0 ;;\n";
    script += "esac\n";
    script += "exit 0\n";
    const std::filesystem::path shim = bin / "systemctl";
    write(shim, script);
    ::chmod(shim.c_str(), 0755);
    bin_dir_ = bin;
  }

  // The directory to put on PATH for the shim to be found.
  [[nodiscard]] const std::filesystem::path& bin_dir() const {
    return bin_dir_;
  }
  [[nodiscard]] std::string log() const {
    std::ifstream in(log_);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

 private:
  static void write(const std::filesystem::path& path,
                    const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
  }
  std::filesystem::path state_;
  std::filesystem::path log_;
  std::filesystem::path bin_dir_;
};

StopPolicy fast_policy() {
  return StopPolicy{.stop_timeout = std::chrono::milliseconds(2000),
                    .grace = std::chrono::milliseconds(300),
                    .kill_grace = std::chrono::milliseconds(300),
                    .poll_interval = std::chrono::milliseconds(25)};
}

class ScopedSignals {
 public:
  ~ScopedSignals() { berth::uninstall_exit_signalling(); }
};

}
TEST(build_unit_name_is_prefixed_and_valid) {
  const Result<std::string> name = berth::build_unit_name("berthpanel-memos");
  CHECK(name.is_ok());
  CHECK(name.value() == "berthpanel-build-berthpanel-memos");
  CHECK(berth::validate_unit_name(name.value()));
}

TEST(build_unit_name_refuses_an_invalid_service_name) {
  CHECK(berth::build_unit_name("memos").is_err());  // missing prefix
  CHECK(berth::build_unit_name("").is_err());
}

TEST(build_unit_name_refuses_a_name_past_the_length_cap) {
  // Legal alone, but the "berthpanel-" prefix would push it past the cap.
  const std::string long_service = "berthpanel-" + std::string(101, 'a');
  CHECK(berth::validate_unit_name(long_service));
  const Result<std::string> name = berth::build_unit_name(long_service);
  CHECK(name.is_err());
}

TEST(install_exit_signalling_rejects_a_null_flag) {
  CHECK(berth::install_exit_signalling(nullptr).is_err());
}

TEST(exit_signalling_sets_the_flag_on_sigint_and_sigterm) {
  const ScopedSignals signals_guard;
  std::atomic<bool> cancel{false};
  const Result<void> installed = berth::install_exit_signalling(&cancel);
  CHECK(installed.is_ok());

  CHECK(::raise(SIGINT) == 0);
  CHECK(cancel.load());
  cancel.store(false);

  CHECK(::raise(SIGTERM) == 0);
  CHECK(cancel.load());
}

TEST(terminator_stops_a_running_unit) {
  const TempDir tmp;
  const FakeSystemctl ctl(tmp.path(), "active", "stopped", "stopped");
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path_env("PATH");
  // Shim first on PATH, but it still needs the system's /usr/bin:/bin.
  path_env.set(ctl.bin_dir().string() + ":/usr/bin:/bin");

  SystemdUnitTerminator terminator(fast_policy());
  const Result<bool> stopped = terminator.stop("berthpanel-memos", nullptr);
  CHECK(stopped.is_ok());
  CHECK(stopped.value());

  const std::string log = ctl.log();
  CHECK(log.find("is-active berthpanel-memos.service") != std::string::npos);
  CHECK(log.find("stop berthpanel-memos.service") != std::string::npos);
  CHECK(log.find("SIGKILL") == std::string::npos);  // no escalation needed
}

TEST(terminator_stops_an_activating_auto_restart_unit) {
  // is-active exits 3 for "activating" (a crash-loop), which is NOT "inactive":
  // cleanup must still stop such a unit.
  const TempDir tmp;
  const FakeSystemctl ctl(tmp.path(), "activating", "stopped", "stopped");
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path_env("PATH");
  path_env.set(ctl.bin_dir().string() + ":/usr/bin:/bin");

  SystemdUnitTerminator terminator(fast_policy());
  const Result<bool> stopped = terminator.stop("berthpanel-memos", nullptr);
  CHECK(stopped.is_ok());
  CHECK(stopped.value());

  const std::string log = ctl.log();
  CHECK(log.find("is-active berthpanel-memos.service") != std::string::npos);
  CHECK(log.find("stop berthpanel-memos.service") != std::string::npos);
}

TEST(terminator_escalates_to_sigkill_when_sigterm_is_ignored) {
  const TempDir tmp;
  // stop "succeeds" but the unit stays active; only the kill lands.
  const FakeSystemctl ctl(tmp.path(), "active", "active", "stopped");
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path_env("PATH");
  // Shim first on PATH, but it still needs the system's /usr/bin:/bin.
  path_env.set(ctl.bin_dir().string() + ":/usr/bin:/bin");

  SystemdUnitTerminator terminator(fast_policy());
  const Result<bool> stopped = terminator.stop("berthpanel-memos", nullptr);
  CHECK(stopped.is_ok());
  CHECK(stopped.value());

  const std::string log = ctl.log();
  CHECK(log.find("stop berthpanel-memos.service") != std::string::npos);
  CHECK(log.find("kill --signal=SIGKILL berthpanel-memos.service") !=
        std::string::npos);
}

TEST(terminator_fails_when_the_unit_survives_sigkill) {
  const TempDir tmp;
  const FakeSystemctl ctl(tmp.path(), "active", "active", "active");
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path_env("PATH");
  // Shim first on PATH, but it still needs the system's /usr/bin:/bin.
  path_env.set(ctl.bin_dir().string() + ":/usr/bin:/bin");

  SystemdUnitTerminator terminator(fast_policy());
  const Result<bool> stopped = terminator.stop("berthpanel-memos", nullptr);
  CHECK(stopped.is_err());
  CHECK(stopped.error().ctx().find("survived SIGTERM and SIGKILL") !=
        std::string::npos);
}

TEST(terminator_is_a_noop_for_an_inactive_unit) {
  const TempDir tmp;
  const FakeSystemctl ctl(tmp.path(), "inactive", "stopped", "stopped");
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path_env("PATH");
  // Shim first on PATH, but it still needs the system's /usr/bin:/bin.
  path_env.set(ctl.bin_dir().string() + ":/usr/bin:/bin");

  SystemdUnitTerminator terminator(fast_policy());
  const Result<bool> stopped = terminator.stop("berthpanel-memos", nullptr);
  CHECK(stopped.is_ok());
  CHECK(!stopped.value());
  CHECK(ctl.log().find("stop ") == std::string::npos);
}

TEST(terminator_refuses_an_invalid_unit_name) {
  const TempDir tmp;
  const FakeSystemctl ctl(tmp.path(), "active", "stopped", "stopped");
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path_env("PATH");
  // Shim first on PATH, but it still needs the system's /usr/bin:/bin.
  path_env.set(ctl.bin_dir().string() + ":/usr/bin:/bin");

  SystemdUnitTerminator terminator(fast_policy());
  CHECK(terminator.stop("memos", nullptr).is_err());  // missing prefix
}

// Opt-in like the sandbox live test, never in CI: a REAL orphan under a REAL
// systemd user manager.
TEST(terminator_kills_a_real_orphan_unit_opt_in) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  if (!(enabled.is_ok() && enabled.value().value_or(false))) {
    return;
  }

  const Result<std::string> stem =
      berth::build_unit_name("berthpanel-killtest");
  CHECK(stem.is_ok());

  // A transient unit outliving its launcher, holding a process that would run
  // for another minute if nobody stopped it.
  const Result<std::string> systemd_run =
      berth::resolve_executable("systemd-run");
  CHECK(systemd_run.is_ok());
  berth::RunLimits limits;
  limits.timeout = std::chrono::milliseconds(10000);
  const berth::Result<berth::RunOutput> started =
      berth::run({systemd_run.value(), "--user", "--unit", stem.value(),
                  "--collect", "sleep", "60"},
                 limits);
  CHECK(started.is_ok());

  SystemdUnitTerminator terminator(fast_policy());
  const Result<bool> stopped = terminator.stop(stem.value(), nullptr);
  CHECK(stopped.is_ok());
  CHECK(stopped.value());

  // is-active exits nonzero for anything but a live unit: really gone.
  const Result<int> verdict =
      berth::systemctl_user_code({"is-active", stem.value() + ".service"},
                                 std::chrono::milliseconds(5000), nullptr);
  CHECK(verdict.is_ok());
  CHECK(verdict.value() != 0);
}
