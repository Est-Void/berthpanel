// src/util/subprocess.cpp — posix_spawn based runner with poll-driven
// capture, deadline and cancellation. See subprocess.h for the contract.

#include "util/subprocess.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <utility>

#include "util/errno.h"
#include "util/fd.h"

namespace berth {

namespace {

struct ActionsGuard {
  posix_spawn_file_actions_t* actions;
  ~ActionsGuard() { posix_spawn_file_actions_destroy(actions); }
};

std::string tail_of(const std::string& s, std::size_t max_len) {
  if (s.size() <= max_len) return s;
  return s.substr(s.size() - max_len);
}

// Cancellation and the deadline are only observed between poll calls, so
// poll never sleeps longer than one slice (dev.md §5: every wait is
// interruptible).
constexpr int kPollSliceMs = 200;

}  // namespace

Result<RunOutput> run(const std::vector<std::string>& argv,
                      const RunLimits& limits) {
  if (argv.empty()) return Error("subprocess: empty argv");
  const std::string& path = argv[0];
  if (path.empty() || path.front() != '/') {
    return Error("subprocess: argv[0] must be an absolute path: " + path);
  }
  if (limits.timeout <= std::chrono::milliseconds::zero()) {
    return Error("subprocess: timeout must be positive: " + path);
  }

  int out_pipe[2];
  int err_pipe[2];
  if (::pipe2(out_pipe, O_CLOEXEC) != 0) {
    return Error("subprocess: pipe: " + errno_text(errno));
  }
  FdGuard out_r(out_pipe[0]);
  FdGuard out_w(out_pipe[1]);
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    return Error("subprocess: pipe: " + errno_text(errno));
  }
  FdGuard err_r(err_pipe[0]);
  FdGuard err_w(err_pipe[1]);

  std::vector<char*> child_argv;
  child_argv.reserve(argv.size() + 1);
  for (const std::string& a : argv) {
    child_argv.push_back(const_cast<char*>(a.c_str()));
  }
  child_argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc != 0) {
    return Error("subprocess: spawn actions init: " + errno_text(rc));
  }
  const ActionsGuard actions_guard{&actions};

  rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                        O_RDONLY, 0);
  if (rc != 0) {
    return Error("subprocess: spawn stdin open: " + errno_text(rc));
  }
  rc = posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
  if (rc != 0) {
    return Error("subprocess: spawn stdout dup: " + errno_text(rc));
  }
  rc = posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
  if (rc != 0) {
    return Error("subprocess: spawn stderr dup: " + errno_text(rc));
  }

  std::vector<std::string> env_storage;
  std::vector<char*> envp;
  char** child_env = environ;
  if (limits.extra_env != nullptr && !limits.extra_env->empty()) {
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
      env_storage.emplace_back(*e);
    }
    for (const std::string& entry : *limits.extra_env) {
      const std::size_t eq = entry.find('=');
      if (eq == std::string::npos || eq == 0) {
        return Error("subprocess: malformed extra_env entry: " + entry);
      }
      const std::string_view key(entry.data(), eq);
      bool replaced = false;
      for (std::string& existing : env_storage) {
        const std::size_t existing_eq = existing.find('=');
        if (existing_eq != std::string::npos &&
            std::string_view(existing.data(), existing_eq) == key) {
          existing = entry;
          replaced = true;
          break;
        }
      }
      if (!replaced) env_storage.push_back(entry);
    }
    envp.reserve(env_storage.size() + 1);
    for (std::string& entry : env_storage) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    child_env = envp.data();
  }

  pid_t pid = -1;
  rc = posix_spawn(&pid, path.c_str(), &actions, nullptr, child_argv.data(),
                   child_env);
  if (rc != 0) {
    return Error("subprocess: spawn " + path + ": " + errno_text(rc));
  }

  out_w.close_now();
  err_w.close_now();

  RunOutput output;
  const auto deadline = std::chrono::steady_clock::now() + limits.timeout;

  auto kill_and_fail = [&](const std::string& reason) -> Error {
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    std::string msg = "subprocess: " + reason + ": " + path;
    const std::string tail = tail_of(output.err, 512);
    if (!tail.empty()) msg += ": " + tail;
    return Error(std::move(msg));
  };

  auto pump = [&](int fd, std::string& sink) -> bool {
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (sink.size() < limits.max_capture) {
        const std::size_t room = limits.max_capture - sink.size();
        const std::size_t take = std::min(room, static_cast<std::size_t>(n));
        sink.append(buf, take);
        if (take < static_cast<std::size_t>(n)) output.truncated = true;
      } else {
        output.truncated = true;
      }
      return false;
    }
    if (n == 0) return true;
    if (errno == EINTR) return false;
    return true;
  };

  struct pollfd fds[2] = {{out_r.get(), POLLIN, 0}, {err_r.get(), POLLIN, 0}};
  bool out_eof = false;
  bool err_eof = false;
  while (!out_eof || !err_eof) {
    if (limits.cancel != nullptr &&
        limits.cancel->load(std::memory_order_relaxed)) {
      return kill_and_fail("cancelled");
    }
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::milliseconds::zero()) {
      return kill_and_fail("timed out after " +
                           std::to_string(limits.timeout.count()) + "ms");
    }
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining);
    const int wait_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(
        remaining_ms.count(), kPollSliceMs));
    const int polled = ::poll(fds, 2, wait_ms);
    if (polled < 0) {
      if (errno == EINTR) continue;
      return kill_and_fail("poll failed: " + errno_text(errno));
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      out_eof = pump(out_r.get(), output.out);
    }
    if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      err_eof = pump(err_r.get(), output.err);
    }
    if (out_eof) fds[0].fd = -1;
    if (err_eof) fds[1].fd = -1;
  }

  int status = 0;
  while (true) {
    if (limits.cancel != nullptr &&
        limits.cancel->load(std::memory_order_relaxed)) {
      return kill_and_fail("cancelled");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return kill_and_fail("timed out after " +
                           std::to_string(limits.timeout.count()) + "ms");
    }
    const pid_t done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (done < 0) {
      if (errno == EINTR) continue;
      return Error("subprocess: waitpid " + path + ": " + errno_text(errno));
    }
    timespec ts{0, 10 * 1000 * 1000};
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
  }

  if (WIFEXITED(status)) {
    output.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    output.signaled = true;
    output.term_sig = WTERMSIG(status);
  } else {
    return Error("subprocess: undecodable wait status for " + path);
  }
  return output;
}

}  // namespace berth
