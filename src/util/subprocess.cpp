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
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "util/errno.h"
#include "util/fd.h"
#include "util/sleep.h"

extern char** environ;

namespace berth {

namespace {

// Cancellation and the deadline are only observed between poll calls, so
// poll never sleeps longer than one slice (dev.md §5: every wait is
// interruptible).
constexpr int kPollSliceMs = 200;
constexpr int kWaitSliceMs = 10;

// The child's four pipe ends. The write ends are closed the moment the child
// exists, so poll sees EOF when it exits instead of hanging forever.
struct ChildPipes {
  FdGuard out_read;
  FdGuard out_write;
  FdGuard err_read;
  FdGuard err_write;
};

// Owns the spawn action list from the moment it is initialised, so an early
// return while wiring the pipes cannot leak it.
class SpawnActions {
 public:
  SpawnActions() = default;
  SpawnActions(const SpawnActions&) = delete;
  SpawnActions& operator=(const SpawnActions&) = delete;
  ~SpawnActions() {
    if (ready_)
      posix_spawn_file_actions_destroy(&actions_);
  }

  [[nodiscard]] posix_spawn_file_actions_t* get() { return &actions_; }

  // `cwd` is applied INSIDE wire() on purpose: the list is init'd here, and
  // posix_spawn_file_actions_init() discards anything added before it — a
  // chdir added by the caller first is silently lost, which is exactly the kind
  // of bug that looks like the child ignored the argument.
  [[nodiscard]] Result<void> wire(const int out_fd, const int err_fd,
                                  const std::filesystem::path* cwd);

 private:
  posix_spawn_file_actions_t actions_{};
  bool ready_ = false;
};

Result<ChildPipes> make_pipes() {
  int out[2];
  if (::pipe2(out, O_CLOEXEC) != 0) {
    return Error("subprocess: pipe: " + errno_text(errno));
  }
  int err[2];
  if (::pipe2(err, O_CLOEXEC) != 0) {
    const std::string why = errno_text(errno);
    ::close(out[0]);
    ::close(out[1]);
    return Error("subprocess: pipe: " + why);
  }
  ChildPipes pipes;
  pipes.out_read = FdGuard(out[0]);
  pipes.out_write = FdGuard(out[1]);
  pipes.err_read = FdGuard(err[0]);
  pipes.err_write = FdGuard(err[1]);
  return pipes;
}

Result<void> check_request(const std::vector<std::string>& argv,
                           const RunLimits& limits) {
  if (argv.empty())
    return Error("subprocess: empty argv");
  if (argv.front().empty() || argv.front().front() != '/') {
    return Error("subprocess: argv[0] must be an absolute path: " +
                 argv.front());
  }
  if (limits.timeout <= std::chrono::milliseconds::zero()) {
    return Error("subprocess: timeout must be positive: " + argv.front());
  }
  return {};
}

Result<std::vector<char*>> child_argv(const std::vector<std::string>& argv) {
  std::vector<char*> out;
  out.reserve(argv.size() + 1);
  for (const std::string& a : argv) {
    out.push_back(const_cast<char*>(a.c_str()));
  }
  out.push_back(nullptr);
  return out;
}

// Merges extra_env over the inherited environment. An entry without '=' or
// with an empty name is refused rather than passed through: the child would
// ignore it, or read the rest as the value (§11: empty means unset).
Result<std::vector<std::string>> merged_env(const RunLimits& limits) {
  std::vector<std::string> storage;
  if (limits.extra_env == nullptr || limits.extra_env->empty())
    return storage;

  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    storage.emplace_back(*e);
  }
  for (const std::string& entry : *limits.extra_env) {
    const std::size_t eq = entry.find('=');
    if (eq == std::string::npos || eq == 0) {
      return Error("subprocess: malformed extra_env entry: " + entry);
    }
    const std::string_view key(entry.data(), eq);
    bool replaced = false;
    for (std::string& existing : storage) {
      const std::size_t existing_eq = existing.find('=');
      if (existing_eq == std::string::npos)
        continue;
      if (std::string_view(existing.data(), existing_eq) != key)
        continue;
      existing = entry;
      replaced = true;
      break;
    }
    if (!replaced)
      storage.push_back(entry);
  }
  return storage;
}

// Fills `out` with pointers into `storage`. The caller owns `out`: the
// pointers stay valid only while it is alive.
void env_pointers(std::vector<std::string>& storage, std::vector<char*>* out) {
  out->reserve(storage.size() + 1);
  for (std::string& entry : storage) {
    out->push_back(entry.data());
  }
  out->push_back(nullptr);
}
Result<void> add_dup2(posix_spawn_file_actions_t* actions, const int from,
                      const int to) {
  if (posix_spawn_file_actions_adddup2(actions, from, to) != 0) {
    return Error("subprocess: spawn dup2: " + errno_text(errno));
  }
  return {};
}

// chdir BEFORE the dup2s: posix_spawn applies the file actions in order, and
// the pipe ends must be in place before the child starts talking.
Result<void> add_chdir(posix_spawn_file_actions_t* actions,
                       const std::filesystem::path& cwd) {
  if (::posix_spawn_file_actions_addchdir_np(actions, cwd.c_str()) != 0) {
    return Error("subprocess: chdir into " + cwd.string() +
                 " failed: " + errno_text(errno));
  }
  return {};
}

Result<void> SpawnActions::wire(const int out_fd, const int err_fd,
                                const std::filesystem::path* cwd) {
  if (posix_spawn_file_actions_init(&actions_) != 0) {
    return Error("subprocess: spawn actions init: " + errno_text(errno));
  }
  ready_ = true;
  if (posix_spawn_file_actions_addopen(&actions_, STDIN_FILENO, "/dev/null",
                                       O_RDONLY, 0) != 0) {
    return Error("subprocess: spawn stdin open: " + errno_text(errno));
  }
  if (cwd != nullptr && !cwd->empty()) {
    // A missing directory is the caller's mistake and must be named BEFORE the
    // child runs, not surfaced as whatever the child happened to print.
    std::error_code ec;
    if (!std::filesystem::is_directory(*cwd, ec)) {
      return Error("subprocess: working directory " + cwd->string() +
                   " is not a directory: " + ec.message());
    }
    const Result<void> chdir = add_chdir(&actions_, *cwd);
    if (chdir.is_err())
      return chdir;
  }
  const Result<void> out = add_dup2(&actions_, out_fd, STDOUT_FILENO);
  if (out.is_err())
    return out;
  return add_dup2(&actions_, err_fd, STDERR_FILENO);
}

Result<pid_t> spawn_child(const std::string& path,
                          const std::vector<char*>& argv, char** envp,
                          posix_spawn_file_actions_t* actions) {
  pid_t pid = -1;
  const int rc =
      posix_spawn(&pid, path.c_str(), actions, nullptr, argv.data(), envp);
  if (rc != 0) {
    return Error("subprocess: spawn " + path + ": " + errno_text(rc));
  }
  return pid;
}

// Owns the child between spawn and reap: every early return below goes
// through kill(), so no failure path can leave a process behind.
class ChildGuard {
 public:
  ChildGuard(const pid_t pid, std::string path)
      : pid_(pid), path_(std::move(path)) {}
  ChildGuard(const ChildGuard&) = delete;
  ChildGuard& operator=(const ChildGuard&) = delete;

  [[nodiscard]] pid_t pid() const { return pid_; }

  [[nodiscard]] Error kill(const std::string& reason,
                           const std::string& stderr_tail) const {
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    std::string msg = "subprocess: " + reason + ": " + path_;
    if (!stderr_tail.empty())
      msg += ": " + stderr_tail;
    return Error(std::move(msg));
  }

 private:
  pid_t pid_;
  std::string path_;
};

// Everything the two wait loops below share: who to kill, how long to keep
// waiting, and where the stderr tail for the failure message comes from.
struct Wait {
  const ChildGuard* child = nullptr;
  const RunLimits* limits = nullptr;
  std::chrono::steady_clock::time_point deadline{};
  RunOutput* output = nullptr;
};

[[nodiscard]] bool cancelled(const RunLimits& limits) {
  return limits.cancel != nullptr &&
         limits.cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] std::string timeout_reason(const RunLimits& limits) {
  return "timed out after " + std::to_string(limits.timeout.count()) + "ms";
}

std::string tail_of(const std::string& s, const std::size_t max_len) {
  if (s.size() <= max_len)
    return s;
  return s.substr(s.size() - max_len);
}

// The single abort point for both loops. Returns an Error that has already
// killed the child.
Result<void> check_alive(const Wait& wait) {
  if (cancelled(*wait.limits)) {
    return wait.child->kill("cancelled", tail_of(wait.output->err, 512));
  }
  if (std::chrono::steady_clock::now() >= wait.deadline) {
    return wait.child->kill(timeout_reason(*wait.limits),
                            tail_of(wait.output->err, 512));
  }
  return {};
}

// One read from a capture pipe. Returns true at EOF; a full buffer stops
// appending but keeps draining, so the child never blocks on a full pipe.
bool pump(const int fd, std::string& sink, const std::size_t cap,
          bool* truncated) {
  char buf[4096];
  const ssize_t n = ::read(fd, buf, sizeof(buf));
  if (n > 0) {
    if (sink.size() < cap) {
      const std::size_t take =
          std::min(cap - sink.size(), static_cast<std::size_t>(n));
      sink.append(buf, take);
      if (take < static_cast<std::size_t>(n))
        *truncated = true;
    } else {
      *truncated = true;
    }
    return false;
  }
  if (n == 0)
    return true;
  return errno != EINTR;
}

Result<void> drain_pipes(ChildPipes& pipes, const Wait& wait) {
  struct pollfd fds[2] = {
      {pipes.out_read.get(), POLLIN, 0},
      {pipes.err_read.get(), POLLIN, 0},
  };
  bool out_eof = false;
  bool err_eof = false;
  while (!out_eof || !err_eof) {
    const Result<void> alive = check_alive(wait);
    if (alive.is_err())
      return std::move(alive).error();

    const int polled = ::poll(fds, 2, kPollSliceMs);
    if (polled < 0) {
      if (errno == EINTR)
        continue;
      return Error("subprocess: poll: " + errno_text(errno));
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      out_eof = pump(fds[0].fd, wait.output->out, wait.limits->max_capture,
                     &wait.output->truncated);
    }
    if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      err_eof = pump(fds[1].fd, wait.output->err, wait.limits->max_capture,
                     &wait.output->truncated);
    }
    if (out_eof)
      fds[0].fd = -1;
    if (err_eof)
      fds[1].fd = -1;
  }
  return {};
}

Result<int> reap(const Wait& wait) {
  int status = 0;
  while (true) {
    const Result<void> alive = check_alive(wait);
    if (alive.is_err())
      return std::move(alive).error();

    const pid_t done = ::waitpid(wait.child->pid(), &status, WNOHANG);
    if (done == wait.child->pid())
      return status;
    if (done < 0) {
      if (errno == EINTR)
        continue;
      return Error("subprocess: waitpid: " + errno_text(errno));
    }
    sleep_ms(kWaitSliceMs);
  }
}

// Capture first, then reap: both pipes must reach EOF before a nonblocking
// waitpid can be trusted to report the exit status promptly.
Result<int> await_child(const ChildGuard& child, ChildPipes& pipes,
                        const RunLimits& limits, RunOutput* output) {
  Wait wait;
  wait.child = &child;
  wait.limits = &limits;
  wait.deadline = std::chrono::steady_clock::now() + limits.timeout;
  wait.output = output;

  const Result<void> drained = drain_pipes(pipes, wait);
  if (drained.is_err())
    return std::move(drained).error();
  return reap(wait);
}

Result<void> decode_status(const int status, const std::string& path,
                           RunOutput* output) {
  if (WIFEXITED(status)) {
    output->exit_code = WEXITSTATUS(status);
    return {};
  }
  if (WIFSIGNALED(status)) {
    output->signaled = true;
    output->term_sig = WTERMSIG(status);
    return {};
  }
  return Error("subprocess: undecodable wait status for " + path);
}

}  // namespace

Result<RunOutput> run(const std::vector<std::string>& argv,
                      const RunLimits& limits) {
  const Result<void> request = check_request(argv, limits);
  if (request.is_err())
    return std::move(request).error();

  Result<ChildPipes> made = make_pipes();
  if (made.is_err())
    return std::move(made).error();
  ChildPipes pipes = std::move(made).value();

  SpawnActions actions;
  const Result<void> wired =
      actions.wire(pipes.out_write.get(), pipes.err_write.get(), limits.cwd);
  if (wired.is_err())
    return std::move(wired).error();

  const Result<std::vector<char*>> argv_ptrs = child_argv(argv);
  if (argv_ptrs.is_err())
    return std::move(argv_ptrs).error();

  Result<std::vector<std::string>> merged = merged_env(limits);
  if (merged.is_err())
    return std::move(merged).error();
  std::vector<std::string> env_storage = std::move(merged).value();
  std::vector<char*> env_ptrs;
  char** child_env = environ;
  if (!env_storage.empty()) {
    env_pointers(env_storage, &env_ptrs);
    child_env = env_ptrs.data();
  }

  const Result<pid_t> spawned =
      spawn_child(argv.front(), argv_ptrs.value(), child_env, actions.get());
  if (spawned.is_err())
    return std::move(spawned).error();

  pipes.out_write.close_now();
  pipes.err_write.close_now();

  const ChildGuard child(spawned.value(), argv.front());
  RunOutput output;
  const Result<int> status = await_child(child, pipes, limits, &output);
  if (status.is_err())
    return std::move(status).error();

  const Result<void> decoded =
      decode_status(status.value(), argv.front(), &output);
  if (decoded.is_err())
    return std::move(decoded).error();
  return output;
}

}  // namespace berth
