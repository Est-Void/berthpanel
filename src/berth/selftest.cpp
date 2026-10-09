// src/berth/selftest.cpp — see selftest.h for the contract.

#include "selftest.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "berth/journal.h"
#include "berth/port.h"
#include "core/atomic.h"
#include "core/names.h"
#include "core/state.h"
#include "util/errno.h"
#include "util/fs.h"

namespace berth {

namespace {

constexpr std::chrono::milliseconds kProbeTimeout{5000};
constexpr std::chrono::milliseconds kActiveBudget{15000};
constexpr std::chrono::milliseconds kHttpBudget{8000};
constexpr std::chrono::milliseconds kPollInterval{200};
constexpr std::size_t kMaxNameAttempts = 16;

bool cancelled(std::atomic<bool>* flag) {
  return flag != nullptr && flag->load();
}

// A random-suffixed scratch dir under the real data dir, removed on every
// exit path. NOT TempDir: TempDir is /tmp-only, and the unit's PrivateTmp=yes
// makes a /tmp work dir invisible inside the unit namespace.
class Scratch {
 public:
  Scratch() = default;
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  ~Scratch() {
    const Result<void> removed = remove();
    static_cast<void>(removed);
  }

  Result<void> create(const std::filesystem::path& base,
                      const std::string& suffix) {
    dir_ = base / ("selftest-" + suffix);
    std::error_code ec;
    if (std::filesystem::exists(dir_, ec)) {
      return Error("selftest: scratch already exists: " + dir_.string());
    }
    return ensure_dir(dir_);
  }

  Result<void> remove() {
    if (removed_)
      return {};
    removed_ = true;
    if (dir_.empty())
      return {};
    const Result<std::size_t> removed = remove_tree(dir_);
    if (removed.is_err())
      return std::move(removed).error();
    return {};
  }

  [[nodiscard]] const std::filesystem::path& path() const { return dir_; }

 private:
  std::filesystem::path dir_;
  bool removed_ = false;
};

// Removes the unit the selftest created, on every path. Idempotent; never
// polls a cancel flag (cleanup must run even after SIGINT).
class UnitGuard {
 public:
  UnitGuard(Paths paths, std::string name)
      : paths_(std::move(paths)), name_(std::move(name)) {}
  UnitGuard(const UnitGuard&) = delete;
  UnitGuard& operator=(const UnitGuard&) = delete;
  ~UnitGuard() {
    const Result<void> cleaned = cleanup();
    static_cast<void>(cleaned);
  }

  Result<void> cleanup() {
    if (cleaned_)
      return {};
    const std::string unit = name_ + ".service";
    std::string failures;
    const auto attempt = [&](const std::vector<std::string>& args) {
      const Result<int> code =
          systemctl_user_code(args, std::chrono::milliseconds(15000), nullptr);
      if (code.is_err())
        failures += code.error().ctx() + "; ";
    };
    attempt({"stop", unit});
    attempt({"reset-failed", unit});
    attempt({"disable", unit});
    std::error_code ec;
    std::filesystem::remove(paths_.units_dir / unit, ec);
    if (ec)
      failures += "remove unit file: " + ec.message() + "; ";
    const Result<void> reload = systemctl_daemon_reload();
    if (reload.is_err())
      failures += reload.error().ctx() + "; ";
    cleaned_ = true;
    if (!failures.empty())
      return Error("selftest cleanup: " + failures);
    return {};
  }

 private:
  Paths paths_;
  std::string name_;
  bool cleaned_ = false;
};

Result<std::string> http_get_loopback(const std::uint16_t port,
                                      const std::string& path) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return Error("selftest: socket: " + errno_text(errno));
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } closing{fd};

  timeval timeout{};
  timeout.tv_sec = 3;
  static_cast<void>(
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
  static_cast<void>(
      ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    return Error("selftest: connect 127.0.0.1:" + std::to_string(port) + ": " +
                 errno_text(errno));
  }

  const std::string request = selftest_http_request(path);
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n =
        ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (n <= 0)
      return Error("selftest: send: " + errno_text(errno));
    sent += static_cast<std::size_t>(n);
  }
  static_cast<void>(::shutdown(fd, SHUT_WR));

  std::string wire;
  char buffer[4096];
  while (true) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return Error("selftest: recv: " + errno_text(errno));
    }
    if (n == 0)
      break;
    wire.append(buffer, static_cast<std::size_t>(n));
  }
  return wire;
}

std::set<std::string> service_names(const std::vector<ServiceRecord>& records) {
  std::set<std::string> names;
  for (const ServiceRecord& record : records) names.insert(record.service_name);
  return names;
}

}  // namespace

bool SelftestReport::ok() const {
  for (const SelftestStep& step : steps) {
    if (!step.ok)
      return false;
  }
  return leftovers_free;
}

std::string selftest_unit_name(const std::string_view suffix) {
  return std::string(kUnitPrefix) + "selftest-" + std::string(suffix);
}

Result<std::string> selftest_random_suffix() {
  std::random_device rd;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(8);
  for (int i = 0; i < 8; ++i)
    out.push_back(kHex[static_cast<std::size_t>(rd() & 0xFu)]);
  return out;
}

std::string selftest_http_request(const std::string_view path) {
  return "GET " + std::string(path) +
         " HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
}

int parse_http_status(const std::string_view wire) {
  if (wire.rfind("HTTP/1.", 0) != 0)
    return -1;
  const std::size_t space = wire.find(' ');
  if (space == std::string_view::npos || space + 4 > wire.size())
    return -1;
  int code = 0;
  for (std::size_t i = space + 1; i < space + 4; ++i) {
    const char c = wire[i];
    if (c < '0' || c > '9')
      return -1;
    code = code * 10 + (c - '0');
  }
  return code;
}

std::string parse_http_body(const std::string_view wire) {
  const std::size_t split = wire.find("\r\n\r\n");
  if (split == std::string_view::npos)
    return {};
  return std::string(wire.substr(split + 4));
}

Result<UnitSpec> selftest_unit_spec(const std::string& unit_name,
                                    const std::filesystem::path& binary,
                                    const std::filesystem::path& root,
                                    const std::uint16_t port) {
  if (binary.empty() || !binary.is_absolute())
    return Error("selftest: service binary must be an absolute path");
  if (root.empty() || !root.is_absolute())
    return Error("selftest: service root must be an absolute path");
  UnitSpec spec;
  spec.service_name = unit_name;
  spec.description = "berthpanel selftest loopback service";
  spec.working_dir = root;
  spec.exec_binary = binary.string();
  spec.exec_args = {"__serve-static", root.string(), "--listen",
                    "127.0.0.1:" + std::to_string(port)};
  spec.limits = UnitLimits{.memory_max = "128M", .tasks_max = 64};
  spec.read_write_paths = {root};
  return spec;
}

Result<bool> selftest_name_is_free(const Paths& paths,
                                   const std::string_view name) {
  if (!validate_unit_name(name))
    return Error("selftest: refusing an invalid unit name: '" +
                 std::string(name) + "'");
  std::error_code ec;
  const bool unit_exists = std::filesystem::exists(
      paths.units_dir / (std::string(name) + ".service"), ec);
  if (ec)
    return Error("selftest: stat unit file: " + ec.message());
  if (unit_exists)
    return false;
  const Result<std::optional<ServiceRecord>> registered =
      registry_get(paths, name);
  if (registered.is_err())
    return std::move(registered).error();
  return !registered.value().has_value();
}

Result<SelftestReport> run_selftest(const Paths& paths,
                                    const std::filesystem::path& binary,
                                    std::atomic<bool>* cancel) {
  SelftestReport report;
  if (binary.empty() || !binary.is_absolute())
    return Error("selftest: service binary must be an absolute path");

  const Result<std::vector<ServiceRecord>> registry_before =
      registry_list(paths);
  if (registry_before.is_err())
    return with_context(std::move(registry_before).error(),
                        "selftest: reading the registry before the run");
  const Result<std::vector<JournalEntry>> journal_before = journal_load(paths);
  if (journal_before.is_err())
    return with_context(std::move(journal_before).error(),
                        "selftest: reading the journal before the run");

  // A name that is provably unused: the manager's namespace is shared, so
  // this is the one thing that must be certain before anything is created.
  std::string suffix;
  std::string name;
  bool have_name = false;
  for (std::size_t attempt = 0; attempt < kMaxNameAttempts && !have_name;
       ++attempt) {
    const Result<std::string> candidate = selftest_random_suffix();
    if (candidate.is_err())
      return std::move(candidate).error();
    suffix = candidate.value();
    name = selftest_unit_name(suffix);
    const Result<bool> free = selftest_name_is_free(paths, name);
    if (free.is_err())
      return std::move(free).error();
    have_name = free.value();
  }
  if (!have_name)
    return Error("selftest: could not find a free unit name after " +
                 std::to_string(kMaxNameAttempts) + " attempts");

  Scratch scratch;
  const Result<void> made = scratch.create(paths.data_dir, suffix);
  if (made.is_err())
    return std::move(made).error();

  const std::filesystem::path root = scratch.path() / "www";
  const Result<void> root_dir = ensure_dir(root);
  if (root_dir.is_err())
    return std::move(root_dir).error();
  const std::string marker = "berthpanel-selftest:" + suffix;
  const Result<void> wrote_marker =
      atomic_write(root / "index.html", marker + "\n", kPermShared);
  if (wrote_marker.is_err())
    return std::move(wrote_marker).error();

  const Result<std::uint16_t> port = pick_free_port();
  if (port.is_err())
    return std::move(port).error();

  const Result<UnitSpec> spec =
      selftest_unit_spec(name, binary, root, port.value());
  if (spec.is_err())
    return std::move(spec).error();

  UnitGuard guard(paths, name);
  bool failed = false;
  bool interrupted = false;
  const auto step = [&](std::string step_name, const bool ok,
                        std::string detail) {
    report.steps.push_back(
        SelftestStep{std::move(step_name), ok, std::move(detail)});
  };
  const auto poll = [&](const std::vector<std::string>& args) {
    return systemctl_user_code(args, kProbeTimeout, cancel);
  };

  const Result<std::filesystem::path> unit_file =
      write_unit(paths, spec.value());
  if (unit_file.is_err()) {
    step("render and write the unit file", false, unit_file.error().ctx());
    failed = true;
  } else {
    step("render and write the unit file", true,
         unit_file.value().filename().string());
  }

  if (!failed) {
    const Result<void> reload = systemctl_daemon_reload();
    step("systemctl --user daemon-reload", reload.is_ok(),
         reload.is_err() ? reload.error().ctx() : "");
    failed = reload.is_err();
  }

  if (!failed && !cancelled(cancel)) {
    const Result<int> enabled =
        systemctl_user_code({"enable", "--now", name + ".service"},
                            std::chrono::milliseconds(20000), cancel);
    if (enabled.is_err()) {
      step("systemctl --user enable --now", false, enabled.error().ctx());
      failed = true;
    } else {
      step("systemctl --user enable --now", enabled.value() == 0,
           enabled.value() == 0 ? ""
                                : "exit " + std::to_string(enabled.value()));
      failed = enabled.value() != 0;
    }
  }

  if (!failed) {
    const auto deadline = std::chrono::steady_clock::now() + kActiveBudget;
    bool active = false;
    int last = -1;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancelled(cancel)) {
        interrupted = true;
        break;
      }
      const Result<int> state = poll({"is-active", name + ".service"});
      if (state.is_err()) {
        last = -2;
        break;
      }
      last = state.value();
      if (last == 0) {
        active = true;
        break;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    if (!interrupted) {
      step("the unit becomes active", active,
           active ? ""
                  : "is-active never returned 0 (last " + std::to_string(last) +
                        ")");
      failed = !active;
    }
  }

  if (!failed && !interrupted) {
    const auto deadline = std::chrono::steady_clock::now() + kHttpBudget;
    bool served = false;
    std::string detail;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancelled(cancel)) {
        interrupted = true;
        break;
      }
      const Result<std::string> wire =
          http_get_loopback(port.value(), "/index.html");
      if (wire.is_ok()) {
        const int status = parse_http_status(wire.value());
        const std::string body = parse_http_body(wire.value());
        if (status == 200 && body.find(marker) != std::string::npos) {
          served = true;
          break;
        }
        detail = "status " + std::to_string(status);
      } else {
        detail = wire.error().ctx();
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    if (!interrupted) {
      step("loopback GET serves the marker file", served, served ? "" : detail);
      failed = !served;
    }
  }

  if (interrupted)
    step("the run was not interrupted by a signal", false,
         "SIGINT/SIGTERM arrived; cleanup still ran");

  const Result<void> cleaned = guard.cleanup();
  step("stop, disable and remove the unit", cleaned.is_ok(),
       cleaned.is_err() ? cleaned.error().ctx() : "");

  const Result<void> scratch_removed = scratch.remove();
  step("remove the scratch directory", scratch_removed.is_ok(),
       scratch_removed.is_err() ? scratch_removed.error().ctx() : "");

  bool leftovers_free = true;

  const Result<int> active_after = poll({"is-active", name + ".service"});
  const bool gone = active_after.is_ok() && active_after.value() != 0;
  step("the unit is no longer active", gone,
       active_after.is_err()
           ? active_after.error().ctx()
           : "is-active exit " + std::to_string(active_after.value()));
  leftovers_free = leftovers_free && gone;

  std::error_code ec;
  const bool file_gone =
      !std::filesystem::exists(paths.units_dir / (name + ".service"), ec);
  step("the unit file is gone", file_gone && !ec, ec ? ec.message() : "");
  leftovers_free = leftovers_free && file_gone && !ec;

  const bool scratch_gone = !std::filesystem::exists(scratch.path(), ec);
  step("the scratch directory is gone", scratch_gone && !ec,
       ec ? ec.message() : "");
  leftovers_free = leftovers_free && scratch_gone && !ec;

  const Result<std::vector<ServiceRecord>> registry_after =
      registry_list(paths);
  const bool registry_same =
      registry_after.is_ok() && service_names(registry_after.value()) ==
                                    service_names(registry_before.value());
  step("the registry is unchanged", registry_same,
       registry_after.is_err() ? registry_after.error().ctx() : "");
  leftovers_free = leftovers_free && registry_same;

  const Result<std::vector<JournalEntry>> journal_after = journal_load(paths);
  const bool journal_same =
      journal_after.is_ok() &&
      journal_after.value().size() == journal_before.value().size();
  step("the deploy journal is unchanged", journal_same,
       journal_after.is_err() ? journal_after.error().ctx() : "");
  leftovers_free = leftovers_free && journal_same;

  report.leftovers_free = leftovers_free;
  return report;
}

std::string render_selftest_report(const SelftestReport& report) {
  std::string out;
  for (const SelftestStep& step : report.steps) {
    out += step.ok ? "  ok   " : "  FAIL ";
    out += step.name;
    if (!step.detail.empty()) {
      out += "  (";
      out += step.detail;
      out += ")";
    }
    out += "\n";
  }
  out += "\n  leftovers: ";
  out += report.leftovers_free ? "none\n" : "FOUND — investigate\n";
  out += report.ok() ? "\n  selftest PASS\n" : "\n  selftest FAIL\n";
  return out;
}

}  // namespace berth
