#include "berth/service_ops.h"

#include <string>

#include "berth/cancel.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>

#include "berth/units.h"
#include "core/atomic.h"
#include "core/flags.h"
#include "core/names.h"
#include "core/paths.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"
#include "util/subprocess.h"
#include "util/which.h"

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.state_json = p.state_dir / "state.json";
  p.data_dir = root / "data" / "berthpanel";
  p.services_dir = p.data_dir / "services";
  p.config_dir = root / "config" / "berthpanel";
  p.units_dir = root / "config" / "systemd" / "user";
  return p;
}

berth::ServiceRecord memos() {
  berth::ServiceRecord r;
  r.service_name = "berthpanel-memos";
  r.display_name = "Memos";
  r.description = "Note taking";
  r.repo = "usememos/memos";
  r.commit = "0123456789abcdef0123456789abcdef01234567";
  r.port = 8234;
  return r;
}

// /proc traps: comm may hold spaces and parens, and the line may not parse.
TEST(proc_stat_state_reads_the_state_after_the_last_paren) {
  using berth::proc_stat_state;
  CHECK(proc_stat_state("1234 (memos) S 1234 1 0 0 -1 4194560") == 'S');
  // comm with a space AND parentheses: everything up to the LAST ')' is comm.
  CHECK(proc_stat_state("99 (weird (name) here) T 99 1 0 0 -1 0") == 'T');
  // Stopped and traced-stopped are both "frozen".
  CHECK(proc_stat_state("7 (a) T 7 0 0") == 'T');
  CHECK(proc_stat_state("7 (a) t 7 0 0") == 't');
  // A line we cannot parse must NOT read as running.
  CHECK(proc_stat_state("") == '\0');
  CHECK(proc_stat_state("no parens here S 1") == '\0');
  CHECK(proc_stat_state("1234 (truncated") == '\0');
  CHECK(proc_stat_state("1234 (memos) ? 1 2") == '\0');
  // Ending at the space after ')' leaves no state char; close+2 is past end.
  CHECK(proc_stat_state("7 (a) ") == '\0');
  CHECK(proc_stat_state("7 (a)") == '\0');
}

}
TEST(service_ops_unit_file_stays_inside_units_dir) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  const auto file = berth::unit_file_for(p, "berthpanel-memos");
  CHECK(file.is_ok());
  CHECK(file.value() == p.units_dir / "berthpanel-memos.service");
}

TEST(service_ops_refuses_names_that_could_escape_units_dir) {
  // The remover must validate too: a hand-edited registry entry would otherwise
  // delete outside the unit directory (§11).
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  for (const char* bad :
       {"../evil", "berthpanel-../../etc/passwd", "/etc/passwd", "memos", "",
        "berthpanel-a/b", "berthpanel-", "berthpanel-x;rm"}) {
    const auto file = berth::unit_file_for(p, bad);
    CHECK(file.is_err());
    CHECK(file.error().ctx().find("invalid service name") != std::string::npos);
  }
}

TEST(service_ops_accepted_names_cannot_add_a_path_component) {
  // '.' is legal in a stem, so "berthpanel-." is a legal name that must still
  // resolve to a file DIRECTLY inside units_dir, never a component above it.
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());

  for (const char* name : {"berthpanel-.", "berthpanel-..", "berthpanel-a.b.c",
                           "berthpanel-a@inst", "berthpanel-_.9"}) {
    const auto file = berth::unit_file_for(p, name);
    CHECK(file.is_ok());
    CHECK(file.value().parent_path() == p.units_dir);
    CHECK(file.value().filename().string().find('/') == std::string::npos);
  }
}

TEST(service_ops_start_and_stop_refuse_invalid_names_before_spawning) {
  // Validation lives here, not only in units.cpp: never systemctl a bad name.
  for (const char* bad : {"../evil", "memos", ""}) {
    CHECK(berth::service_start(bad).is_err());
    CHECK(berth::service_stop(bad).is_err());
    CHECK(berth::service_uninstall(test_paths("/tmp/unused"), bad).is_err());
  }
}

TEST(service_ops_rows_report_the_registry_with_short_commits) {
  // No unit for a TempDir registry: rows come back unknown, never vanish (§11).
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, memos()).is_ok());

  std::atomic<bool> cancel{false};
  const auto rows = berth::service_rows(p, &cancel);
  CHECK(rows.is_ok());
  CHECK(rows.value().size() == 1);
  CHECK(rows.value()[0].service_name == "berthpanel-memos");
  CHECK(rows.value()[0].display_name == "Memos");
  CHECK(rows.value()[0].port == 8234);
  CHECK(rows.value()[0].commit_short == "0123456");
}

TEST(service_ops_rows_stop_at_a_cancelled_flag) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, memos()).is_ok());

  std::atomic<bool> cancel{true};
  const auto rows = berth::service_rows(p, &cancel);
  CHECK(rows.is_err());
  CHECK(rows.error().ctx().find("cancelled") != std::string::npos);
}

TEST(service_ops_uninstall_leaves_the_work_dir_and_removes_the_unit_file) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, memos()).is_ok());

  // The work dir holds the user's data; a silent delete is forbidden (§11).
  const std::filesystem::path unit = p.units_dir / "berthpanel-memos.service";
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(berth::atomic_write(unit, "[Service]\n", berth::kPermShared).is_ok());
  const std::filesystem::path work = p.services_dir / "berthpanel-memos";
  CHECK(berth::ensure_dir(work).is_ok());
  CHECK(berth::atomic_write(work / "memos.db", "data", berth::kPermPrivate)
            .is_ok());

  const auto removed = berth::service_uninstall(p, "berthpanel-memos");
  // The systemctl half needs a user manager: either all of it ran, or it
  // failed there and nothing was deleted — never the other way round.
  if (removed.is_ok()) {
    CHECK(!std::filesystem::exists(unit));
    CHECK(std::filesystem::exists(work / "memos.db"));
    const auto listed = berth::registry_list(p);
    CHECK(listed.is_ok());
    CHECK(listed.value().empty());
  } else {
    CHECK(std::filesystem::exists(unit));
    CHECK(std::filesystem::exists(work / "memos.db"));
  }
}

TEST(service_ops_live_status_and_stop_opt_in) {
  // Opt-in only, never in CI (§9): touches a real systemd user manager.
  // is a transient, test-only one created and stopped here.
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  if (!(enabled.is_ok() && enabled.value().value_or(false))) {
    return;
  }

  const TempDir tmp;
  berth::Paths p = test_paths(tmp.path());
  berth::ServiceRecord probe = memos();
  probe.service_name = "berthpanel-svcop-probe";
  CHECK(berth::registry_register(p, probe).is_ok());

  // The transient unit IS the record's unit, so the row's is-active query
  // finds it (build_unit_name would namespace it apart).
  const berth::Result<std::string> systemd_run =
      berth::resolve_executable("systemd-run");
  CHECK(systemd_run.is_ok());
  berth::RunLimits limits;
  limits.timeout = std::chrono::milliseconds(10000);
  const berth::Result<berth::RunOutput> started =
      berth::run({systemd_run.value(), "--user", "--unit", probe.service_name,
                  "--collect", "sleep", "60"},
                 limits);
  CHECK(started.is_ok());

  std::atomic<bool> cancel{false};
  auto rows = berth::service_rows(p, &cancel);
  CHECK(rows.is_ok());
  CHECK(rows.value().size() == 1);
  CHECK(rows.value()[0].state == berth::ServiceState::kActive);

  CHECK(berth::service_stop(probe.service_name).is_ok());
  rows = berth::service_rows(p, &cancel);
  CHECK(rows.is_ok());
  // `--collect` unloads a transient unit once it stops, so is-active answers
  // "unknown" rather than "inactive" — the contract is only "not active".
  CHECK(rows.value()[0].state != berth::ServiceState::kActive);
  const berth::Result<int> verdict =
      berth::systemctl_user_code({"is-active", probe.service_name + ".service"},
                                 std::chrono::milliseconds(5000), nullptr);
  CHECK(verdict.is_ok());
  CHECK(verdict.value() != 0);
}

TEST(service_ops_live_uninstall_opt_in) {
  // Opt-in only. The unit file must land in the REAL units_dir or systemd never
  // finds it; the test-only name cannot collide with a deployed service.
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  const auto enabled = berth::env_flag("BERTHPANEL_RUN_SANDBOX_TESTS");
  if (!(enabled.is_ok() && enabled.value().value_or(false))) {
    return;
  }

  const berth::Result<berth::Paths> real = berth::paths_from_env();
  CHECK(real.is_ok());

  const TempDir tmp;
  berth::Paths p = test_paths(tmp.path());
  p.units_dir = real.value().units_dir;  // systemd only looks here
  berth::ServiceRecord probe = memos();
  probe.service_name = "berthpanel-svcop-selftest";
  CHECK(berth::registry_register(p, probe).is_ok());

  const berth::Result<std::filesystem::path> unit =
      berth::unit_file_for(p, probe.service_name);
  CHECK(unit.is_ok());
  CHECK(berth::atomic_write(unit.value(),
                            "[Service]\nExecStart=/usr/bin/sleep 30\n",
                            berth::kPermShared)
            .is_ok());
  CHECK(berth::systemctl_daemon_reload().is_ok());
  const std::filesystem::path work = p.services_dir / probe.service_name;
  CHECK(berth::ensure_dir(work).is_ok());

  CHECK(berth::service_start(probe.service_name).is_ok());
  std::atomic<bool> cancel{false};
  auto rows = berth::service_rows(p, &cancel);
  CHECK(rows.is_ok());
  CHECK(rows.value()[0].state == berth::ServiceState::kActive);

  CHECK(berth::service_uninstall(p, probe.service_name).is_ok());
  CHECK(!std::filesystem::exists(unit.value()));
  CHECK(std::filesystem::exists(work));  // data stays
  const auto listed = berth::registry_list(p);
  CHECK(listed.is_ok());
  CHECK(listed.value().empty());
  const berth::Result<int> verdict =
      berth::systemctl_user_code({"is-active", probe.service_name + ".service"},
                                 std::chrono::milliseconds(5000), nullptr);
  CHECK(verdict.is_ok());
  CHECK(verdict.value() != 0);
}
