// Tests for util/subprocess.h (stage 1.3). Spawns real, tiny processes;
// no network. Contract under test: absolute-path argv, mandatory positive
// timeout, stdout/stderr capture with cap, exit/signal decoding, timeout
// and cancellation kill the child and error loudly.

#include "util/subprocess.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "runner.h"

using berth::Result;
using berth::run;
using berth::RunLimits;
using berth::RunOutput;

namespace {

long long elapsed_ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}
TEST(subprocess_echo_captures_stdout) {
  const Result<RunOutput> r =
      run({"/bin/echo", "hello"},
          RunLimits{.timeout = std::chrono::milliseconds(5000)});
  CHECK(r.is_ok());
  CHECK(r.value().exit_code == 0);
  CHECK(!r.value().signaled);
  CHECK(r.value().out == "hello\n");
  CHECK(r.value().err.empty());
}

TEST(subprocess_exit_code_and_stderr_decoded) {
  const Result<RunOutput> r =
      run({"/bin/sh", "-c", "echo err-line 1>&2; exit 7"},
          RunLimits{.timeout = std::chrono::milliseconds(5000)});
  CHECK(r.is_ok());
  CHECK(r.value().exit_code == 7);
  CHECK(r.value().err == "err-line\n");
}

TEST(subprocess_signal_death_reported) {
  const Result<RunOutput> r =
      run({"/bin/sh", "-c", "kill -9 $$"},
          RunLimits{.timeout = std::chrono::milliseconds(5000)});
  CHECK(r.is_ok());
  CHECK(r.value().signaled);
  CHECK(r.value().term_sig == 9);
  CHECK(r.value().exit_code == -1);
}

TEST(subprocess_rejects_relative_argv0) {
  const Result<RunOutput> r =
      run({"sh", "-c", "echo hi"},
          RunLimits{.timeout = std::chrono::milliseconds(1000)});
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("absolute") != std::string::npos);
}

TEST(subprocess_rejects_empty_argv) {
  const Result<RunOutput> r =
      run({}, RunLimits{.timeout = std::chrono::milliseconds(1000)});
  CHECK(r.is_err());
}

TEST(subprocess_rejects_nonpositive_timeout) {
  const Result<RunOutput> r = run(
      {"/bin/echo", "x"}, RunLimits{.timeout = std::chrono::milliseconds(0)});
  CHECK(r.is_err());
}

TEST(subprocess_nonexistent_binary_errors) {
  const Result<RunOutput> r =
      run({"/nonexistent-berth-test-binary"},
          RunLimits{.timeout = std::chrono::milliseconds(1000)});
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("spawn") != std::string::npos);
}

TEST(subprocess_timeout_kills_child_and_errors) {
  const auto t0 = std::chrono::steady_clock::now();
  const Result<RunOutput> r =
      run({"/bin/sleep", "30"},
          RunLimits{.timeout = std::chrono::milliseconds(150)});
  const long long elapsed = elapsed_ms_since(t0);
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("timed out") != std::string::npos);
  CHECK(elapsed < 10000);
}

TEST(subprocess_cancel_flag_kills_child) {
  std::atomic<bool> cancel{false};
  std::thread settler([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancel.store(true);
  });
  const auto t0 = std::chrono::steady_clock::now();
  const Result<RunOutput> r =
      run({"/bin/sleep", "30"},
          RunLimits{.timeout = std::chrono::milliseconds(30000),
                    .cancel = &cancel});
  const long long elapsed = elapsed_ms_since(t0);
  settler.join();
  CHECK(r.is_err());
  CHECK(r.error().ctx().find("cancelled") != std::string::npos);
  CHECK(elapsed < 10000);
}

TEST(subprocess_capture_capped_and_flagged) {
  const Result<RunOutput> r =
      run({"/usr/bin/seq", "1", "300000"},
          RunLimits{.timeout = std::chrono::milliseconds(30000)});
  CHECK(r.is_ok());
  CHECK(r.value().truncated);
  CHECK(r.value().out.size() == 1024 * 1024);
}
