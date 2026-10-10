#include "berth/deploy.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "analyzer/ports.h"
#include "berth/journal.h"
#include "berth/lock.h"
#include "berth/recipes.h"
#include "core/atomic.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::DeployOps;
using berth::DeployOptions;
using berth::Recipe;
using berth::Result;
using berth::run_deploy;
using berth::ServiceRecord;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.secrets_dir = p.state_dir / "secrets";
  p.services_dir = root / "share" / "berthpanel" / "services";
  p.units_dir = root / "config" / "systemd" / "user";
  p.state_json = p.state_dir / "state.json";
  p.deploy_journal = p.state_dir / "deploy-journal.json";
  p.deploy_lock = p.state_dir / "deploy.lock";
  p.deploy_log = p.state_dir / "deploy.log";
  p.net_log = p.state_dir / "net.log";
  return p;
}

Recipe prebuilt_recipe() {
  Recipe r;
  r.owner = "usememos";
  r.name = "memos";
  r.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  r.language = "prebuilt";
  r.service_name = "berthpanel-memos";
  r.display_name = "Memos";
  r.description = "notes";
  r.prebuilt = berth::PrebuiltBinary{
      .url =
          "https://github.com/usememos/memos/releases/download/v0.31.0/"
          "memos_0.31.0_linux_amd64.tar.gz",
      .sha256 = std::string(64, 'd'),
      .member = "memos"};
  r.binary_name = "memos";
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port} --data {project}/data";
  r.res = {"512M", 300};
  return r;
}

class FakeOps final : public DeployOps {
 public:
  std::vector<std::string> calls;
  bool fail_fetch = false;
  bool fail_build = false;
  bool fail_unit = false;
  bool fail_start = false;
  bool fail_running = false;
  bool fail_listening = false;
  bool fail_register = false;
  bool fail_stop = false;
  std::filesystem::path last_prefetch_dir;

  Result<void> fetch_source(const Recipe& recipe,
                            const std::filesystem::path& work_dir,
                            const berth::Paths& paths,
                            const DeployOptions& options) override {
    static_cast<void>(recipe);
    static_cast<void>(work_dir);
    static_cast<void>(paths);
    static_cast<void>(options);
    calls.push_back("fetch");
    if (fail_fetch)
      return berth::Error("fake fetch failure");
    return {};
  }
  Result<void> prefetch(const Recipe& recipe,
                        const std::filesystem::path& work_dir,
                        const berth::Paths& paths,
                        const DeployOptions& options) override {
    static_cast<void>(recipe);
    static_cast<void>(paths);
    // The checkout must be handed to the prefetch, not a directory of the
    // caller's choosing: a fake that ignores the argument would let the real
    // "go mod download in the wrong directory" bug straight through.
    last_prefetch_dir = work_dir;
    static_cast<void>(options);
    calls.push_back("prefetch");
    return {};
  }
  Result<void> build(const Recipe& recipe,
                     const std::filesystem::path& work_dir, std::uint16_t port,
                     const berth::Paths& paths,
                     const DeployOptions& options) override {
    static_cast<void>(recipe);
    static_cast<void>(work_dir);
    static_cast<void>(port);
    static_cast<void>(paths);
    static_cast<void>(options);
    calls.push_back("build");
    if (fail_build)
      return berth::Error("fake build failure");
    return {};
  }
  Result<std::filesystem::path> write_unit_file(
      const Recipe& recipe, const std::filesystem::path& work_dir,
      std::uint16_t port, const berth::Paths& paths) override {
    static_cast<void>(recipe);
    static_cast<void>(work_dir);
    static_cast<void>(port);
    static_cast<void>(paths);
    calls.push_back("unit");
    if (fail_unit)
      return berth::Error("fake unit failure");
    return work_dir / "memos.service";
  }
  Result<void> start_service(const std::string& service_name) override {
    static_cast<void>(service_name);
    calls.push_back("start");
    if (fail_start)
      return berth::Error("fake start failure");
    return {};
  }
  Result<void> await_running(const std::string& service_name,
                             const berth::Paths& paths,
                             std::atomic<bool>* cancel) override {
    static_cast<void>(service_name);
    static_cast<void>(paths);
    static_cast<void>(cancel);
    calls.push_back("await");
    if (fail_running)
      return berth::Error("unit never became active");
    return {};
  }
  // The real probe reads /proc/net; the fake returns a controlled verdict so
  // run_deploy stays testable without touching real /proc (dev.md §9).
  Result<void> verify_listening(const std::string& service_name,
                                std::uint16_t port, const berth::Paths& paths,
                                std::atomic<bool>* cancel) override {
    static_cast<void>(service_name);
    static_cast<void>(port);
    static_cast<void>(paths);
    static_cast<void>(cancel);
    calls.push_back("listening");
    if (fail_listening)
      return berth::Error("fake not listening on the recorded port");
    return {};
  }
  Result<void> register_service(const ServiceRecord& record,
                                const berth::Paths& paths) override {
    calls.push_back("register");
    if (fail_register)
      return berth::Error("registry write refused");
    return berth::registry_register(paths, record);
  }
  Result<bool> stop_unit(const std::string& unit_stem,
                         std::atomic<bool>* cancel) override {
    static_cast<void>(cancel);
    calls.push_back("stop:" + unit_stem);
    if (fail_stop)
      return berth::Error("fake stop failure");
    return false;  // fake units are never running
  }
};

bool called(const FakeOps& ops, const std::string& step) {
  return std::find(ops.calls.begin(), ops.calls.end(), step) != ops.calls.end();
}

void put_executable(const std::filesystem::path& dir, const std::string& name) {
  CHECK(berth::ensure_dir(dir).is_ok());
  const std::filesystem::path file = dir / name;
  FILE* f = std::fopen(file.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("#!/bin/sh\nexit 0\n", f);
  std::fclose(f);
  CHECK(::chmod(file.c_str(), 0755) == 0);
}

}  // namespace

TEST(deploy_happy_path_runs_steps_in_order_and_registers) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_ok());
  const std::vector<std::string> expected = {"fetch",     "prefetch", "build",
                                             "unit",      "start",    "await",
                                             "listening", "register"};
  CHECK(ops.calls == expected);
  CHECK(outcome.value().port > 0);

  const auto registered = berth::registry_get(p, "berthpanel-memos");
  CHECK(registered.is_ok());
  CHECK(registered.value().has_value());
  CHECK(registered.value()->commit ==
        "2b2192d4e153bd04f1d325b60fd880cf00d68b01");
  CHECK(registered.value()->port == outcome.value().port);

  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().empty());
}

TEST(deploy_registry_failure_is_not_a_successful_deploy) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  ops.fail_register = true;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_err());
  CHECK(outcome.error().ctx().find("REGISTRY WRITE FAILED") !=
        std::string::npos);
  // The journal entry survives: lock free + entry present == interrupted,
  // which is exactly what __cleanup reconciles (§6.7).
  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().size() == 1);
  CHECK(journal.value().front().stage == "registering");
}

TEST(deploy_build_failure_keeps_journal_and_skips_the_rest) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  ops.fail_build = true;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_err());
  CHECK(!called(ops, "unit"));
  CHECK(!called(ops, "register"));
  const auto journal = berth::journal_load(p);
  CHECK(journal.value().size() == 1);
  CHECK(journal.value().front().stage == "building");
}

TEST(deploy_fetch_failure_keeps_journal_and_skips_the_rest) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  ops.fail_fetch = true;
  DeployOptions options;
  CHECK(run_deploy(p, prebuilt_recipe(), 0, ops, options).is_err());
  CHECK(!called(ops, "build"));
  CHECK(!called(ops, "unit"));
  const auto journal = berth::journal_load(p);
  CHECK(journal.value().size() == 1);
  CHECK(journal.value().front().stage == "cloning");
}

TEST(deploy_unit_that_never_comes_up_is_not_a_success) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  ops.fail_running = true;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_err());
  CHECK(outcome.error().ctx().find("never became active") != std::string::npos);
  CHECK(!called(ops, "register"));
  CHECK(berth::journal_load(p).value().size() == 1);
}

TEST(deploy_active_but_not_listening_on_the_recorded_port_is_not_a_success) {
  // The fixed-port app that binds somewhere else: the unit stays up, so the
  // settle window is happy, but the registry is about to record a port nobody
  // serves. That is the §11 silent wrong answer the listen check exists for.
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  ops.fail_listening = true;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 18090, ops, options);
  CHECK(outcome.is_err());
  CHECK(outcome.error().ctx().find("fake not listening") != std::string::npos);
  CHECK(called(ops, "await"));
  CHECK(called(ops, "listening"));
  CHECK(!called(ops, "register"));
  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().size() == 1);
  CHECK(journal.value().front().stage == "registering");
}

TEST(deploy_start_failure_is_reported_and_keeps_journal) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  ops.fail_start = true;
  DeployOptions options;
  CHECK(run_deploy(p, prebuilt_recipe(), 0, ops, options).is_err());
  CHECK(!called(ops, "await"));
  CHECK(!called(ops, "register"));
  CHECK(berth::journal_load(p).value().size() == 1);
}

TEST(deploy_refuses_an_invalid_recipe_before_any_step) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  Recipe broken = prebuilt_recipe();
  broken.commit = "main";  // unpinned
  DeployOptions options;
  const auto outcome = run_deploy(p, broken, 0, ops, options);
  CHECK(outcome.is_err());
  CHECK(ops.calls.empty());  // nothing ran
}

TEST(deploy_honours_an_explicit_port) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 18080, ops, options);
  CHECK(outcome.is_ok());
  CHECK(outcome.value().port == 18080);
}

TEST(deploy_takes_the_deploy_lock_for_the_whole_run) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  const auto held = berth::DeployLock::acquire(p);
  CHECK(held.is_ok());

  FakeOps ops;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_err());
  CHECK(outcome.error().ctx().find("already running") != std::string::npos);
  CHECK(ops.calls.empty());
}

// --- stage 4.17: journal reconciliation ---------------------------------

namespace {

berth::JournalEntry stale_entry(const berth::Paths& p,
                                const std::string& service_name,
                                const std::string& stage) {
  berth::JournalEntry entry;
  entry.service_name = service_name;
  entry.work_dir = (p.services_dir / service_name).string();
  entry.repo = "owner/repo";
  entry.commit = std::string(40, 'a');
  entry.stage = stage;
  entry.started_at_unix = 1;
  return entry;
}

// A unit that reports active and then leaves it is CRASH-LOOPING, and
// `Type=simple` calls a unit active the moment it has spawned the process. The
// deploy that reported success on a wrong start_cmd is the §11 failure reached
// through a different door, so the verdict is its own tested function.
TEST(settle_accepts_only_a_unit_that_stays_active) {
  CHECK(berth::settle_active("berthpanel-x", {"active", "active", "active"})
            .is_ok());
  // The reported case: up, then gone (Restart=on-failure auto-restart).
  const auto left =
      berth::settle_active("berthpanel-x", {"active", "active", "activating"});
  CHECK(left.is_err());
  CHECK(left.error().ctx().find("left 'active'") != std::string::npos);
  CHECK(left.error().ctx().find("activating") != std::string::npos);
  // The error must name the usual cause, or the user goes to the unit logs for
  // a flag typo that the detector warned about on the screen.
  CHECK(left.error().ctx().find("start_cmd") != std::string::npos);
  // A unit that failed outright is the same verdict with a different state.
  CHECK(berth::settle_active("berthpanel-x", {"failed"}).is_err());
  CHECK(berth::settle_active("berthpanel-x", {"active", "failed"}).is_err());
  // No observations at all is NOT evidence of serving: vacuous truth would let
  // a settle window that never polled claim a success.
  const auto none = berth::settle_active("berthpanel-x", {});
  CHECK(none.is_err());
  CHECK(none.error().ctx().find("never confirmed") != std::string::npos);
}

// The panel must show the app's OWN error, not send the user to journalctl, so
// the unit's captured verdict/output is composed into the failure message. The
// function stays pure: the evidence is passed in, never gathered here.
TEST(
    settle_failure_message_carries_the_unit_evidence_and_points_at_the_editor) {
  const std::string evidence =
      "systemd: Result=exit-code ExecMainStatus=2\n"
      "service output: flag provided but not defined: -addr";
  const auto left = berth::settle_active("berthpanel-ascii-live",
                                         {"active", "activating"}, evidence);
  CHECK(left.is_err());
  const std::string message = left.error().ctx();
  CHECK(message.find("left 'active'") != std::string::npos);
  // Point at the edit feature, not the stale "edit the recipe file" wording.
  CHECK(message.find("__recipe-edit berthpanel-ascii-live") !=
        std::string::npos);
  CHECK(message.find("'E' in the panel") != std::string::npos);
  // The captured evidence itself rides along, verbatim.
  CHECK(message.find("Result=exit-code ExecMainStatus=2") != std::string::npos);
  CHECK(message.find("flag provided but not defined: -addr") !=
        std::string::npos);
}

// `systemctl show` prints its own order; the parser must not depend on it and
// must emit the properties in a fixed order.
TEST(systemctl_show_parses_the_failure_properties) {
  const std::string show =
      "ActiveState=activating\nSubState=start\nResult=exit-code\n"
      "ExecMainStatus=2\nExecMainCode=1\nMainPID=0\n";
  CHECK(berth::parse_systemctl_show(show) ==
        "Result=exit-code ExecMainStatus=2 ExecMainCode=1 "
        "ActiveState=activating SubState=start");
  // Absent and unknown properties are omitted, not invented.
  CHECK(berth::parse_systemctl_show("Result=success\nMainPID=0\n") ==
        "Result=success");
  CHECK(berth::parse_systemctl_show("").empty());
  CHECK(berth::parse_systemctl_show("garbage without an equals").empty());
}

TEST(journal_tail_is_bounded_by_lines_and_bytes) {
  // Line cap: the LAST n lines, trailing blank lines dropped.
  CHECK(berth::bound_journal_tail("a\nb\nc\n", 2, 100) == "b\nc");
  CHECK(berth::bound_journal_tail("only", 5, 100) == "only");
  // Byte cap: the last bytes, partial leading line dropped.
  CHECK(berth::bound_journal_tail("aaaa\nbbbb\ncccc", 10, 6) == "cccc");
  // A single over-long line has no newline to cut on: keep the capped slice.
  CHECK(berth::bound_journal_tail("abcdefghij", 10, 4) == "ghij");
  // Caps are upper bounds and zero means "nothing".
  CHECK(berth::bound_journal_tail("x", 0, 10).empty());
  CHECK(berth::bound_journal_tail("x", 5, 0).empty());

  std::string many;
  for (int i = 0; i < 10; ++i) {
    many += "line" + std::to_string(i) + "\n";
  }
  CHECK(berth::bound_journal_tail(many, 3, 1024) == "line7\nline8\nline9");
}

// --- listen verdict: pure decision over a PortReport (no /proc) -----------

berth::ListeningPort listening_at(const std::string& address,
                                  const std::uint16_t port) {
  berth::ListeningPort p;
  p.address = address;
  p.port = port;
  p.ipv6 = address.find(':') != std::string::npos;
  p.loopback = address == "::1" || address.rfind("127.", 0) == 0;
  p.wildcard = address == "0.0.0.0" || address == "::";
  return p;
}

berth::PortReport report_with(std::vector<berth::ListeningPort> ports) {
  berth::PortReport report;
  report.ports = std::move(ports);
  return report;
}

TEST(listen_verdict_accepts_the_loopback_listener_on_the_recorded_port) {
  const berth::PortReport report =
      report_with({listening_at("127.0.0.1", 8090)});
  CHECK(berth::verify_loopback_listener(report, 8090, "berthpanel-x").is_ok());
}

TEST(listen_verdict_counts_ipv6_loopback_as_loopback) {
  // ::1 is loopback too: the app bound loopback over IPv6.
  const berth::PortReport report = report_with({listening_at("::1", 8090)});
  CHECK(berth::verify_loopback_listener(report, 8090, "berthpanel-x").is_ok());
}

TEST(listen_verdict_rejects_a_missing_port_and_names_the_remedy) {
  // The concrete bug: the app hardcodes 8090 while recipe.port is 8080. The
  // verdict must name both and point at `port`/start_cmd, or the user cannot
  // connect the deploy success to the observed mismatch.
  const berth::PortReport report =
      report_with({listening_at("127.0.0.1", 8080)});
  const auto verdict =
      berth::verify_loopback_listener(report, 8090, "berthpanel-x");
  CHECK(verdict.is_err());
  const std::string message = verdict.error().ctx();
  CHECK(message.find("8090") != std::string::npos);  // expected port
  CHECK(message.find("8080") != std::string::npos);  // observed listener
  CHECK(message.find("nothing is listening") != std::string::npos);
  CHECK(message.find("start_cmd") != std::string::npos);
  CHECK(message.find("`port`") != std::string::npos);
}

TEST(listen_verdict_rejects_a_wildcard_only_bind_as_exposure) {
  // 0.0.0.0 on the recorded port is the exposure the loopback invariant
  // forbids: it must fail loudly and say so.
  const berth::PortReport report = report_with({listening_at("0.0.0.0", 8090)});
  const auto verdict =
      berth::verify_loopback_listener(report, 8090, "berthpanel-x");
  CHECK(verdict.is_err());
  const std::string message = verdict.error().ctx();
  CHECK(message.find("wildcard") != std::string::npos);
  CHECK(message.find("exposure") != std::string::npos);
  CHECK(message.find("8090") != std::string::npos);
}

TEST(listen_verdict_rejects_a_nonloopback_specific_bind) {
  const berth::PortReport report =
      report_with({listening_at("192.168.1.5", 8090)});
  const auto verdict =
      berth::verify_loopback_listener(report, 8090, "berthpanel-x");
  CHECK(verdict.is_err());
  CHECK(verdict.error().ctx().find("exposed") != std::string::npos);
}

TEST(listen_verdict_rejects_an_empty_or_observably_absent_report) {
  // An empty report is "observably absent", never a silent pass.
  const berth::PortReport empty;
  const auto verdict =
      berth::verify_loopback_listener(empty, 8090, "berthpanel-x");
  CHECK(verdict.is_err());
  CHECK(verdict.error().ctx().find("8090") != std::string::npos);
  CHECK(verdict.error().ctx().find("(none)") != std::string::npos);

  // A listener on a DIFFERENT port is not the recorded port either.
  const berth::PortReport other = report_with({listening_at("0.0.0.0", 9999)});
  CHECK(berth::verify_loopback_listener(other, 8090, "berthpanel-x").is_err());
}

TEST(listen_verdict_accepts_a_loopback_listener_among_other_ports) {
  const berth::PortReport report = report_with({
      listening_at("0.0.0.0", 9999),
      listening_at("127.0.0.1", 8090),
  });
  CHECK(berth::verify_loopback_listener(report, 8090, "berthpanel-x").is_ok());
}

}  // namespace

TEST(deploy_sweeps_stale_journal_entries_before_starting) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-old", "building"))
            .is_ok());
  // Leftovers from the interrupted deploy: work dir and a written unit.
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-old").is_ok());
  CHECK(berth::atomic_write(p.services_dir / "berthpanel-old" / "artifact",
                            "stale")
            .is_ok());
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(berth::atomic_write(p.units_dir / "berthpanel-old.service", "[Unit]\n")
            .is_ok());

  FakeOps ops;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_ok());
  // The sweep runs first, stopping both the app unit and the orphaned
  // build unit, before any deploy step runs.
  const std::vector<std::string> expected = {
      "stop:berthpanel-old",
      "stop:berthpanel-build-berthpanel-old",
      "fetch",
      "prefetch",
      "build",
      "unit",
      "start",
      "await",
      "listening",
      "register"};
  CHECK(ops.calls == expected);
  CHECK(!std::filesystem::exists(p.services_dir / "berthpanel-old"));
  CHECK(!std::filesystem::exists(p.units_dir / "berthpanel-old.service"));
  CHECK(berth::journal_load(p).value().empty());
}

TEST(deploy_refuses_to_start_when_its_own_stale_entry_cannot_be_cleaned) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-memos", "building"))
            .is_ok());

  FakeOps ops;
  ops.fail_stop = true;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_err());
  CHECK(outcome.error().ctx().find("could not be cleaned up") !=
        std::string::npos);
  // The app unit stop failed, so the sweep stopped there: no build-unit
  // stop, no work-dir removal, and no deploy step ran.
  CHECK(ops.calls.size() == 1);
  CHECK(called(ops, "stop:berthpanel-memos"));
  CHECK(!called(ops, "fetch"));
  // The entry stays: the interrupted deploy is still not reconciled.
  CHECK(berth::journal_load(p).value().size() == 1);
}

TEST(deploy_continues_when_another_service_cannot_be_cleaned) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-other", "cloning"))
            .is_ok());

  FakeOps ops;
  ops.fail_stop = true;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_ok());  // one stuck entry must not block new deploys

  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().size() == 1);  // the stuck entry survived
  CHECK(journal.value().front().service_name == "berthpanel-other");

  const auto logged = berth::read_file(p.deploy_log);
  CHECK(logged.is_ok());
  CHECK(logged.value().find("cleanup failed for berthpanel-other") !=
        std::string::npos);
}

TEST(reconcile_interrupted_with_an_empty_journal_is_a_noop) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.empty());
  CHECK(report.value().failed.empty());
  CHECK(!report.value().lock_busy);
  CHECK(ops.calls.empty());
}

TEST(reconcile_interrupted_reports_a_busy_lock_and_touches_nothing) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-memos", "building"))
            .is_ok());
  const auto held = berth::DeployLock::acquire(p);
  CHECK(held.is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().lock_busy);
  CHECK(report.value().swept.empty());
  CHECK(ops.calls.empty());
  CHECK(berth::journal_load(p).value().size() == 1);
}

TEST(reconcile_interrupted_sweeps_when_the_lock_is_free) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p,
                              stale_entry(p, "berthpanel-memos", "registering"))
            .is_ok());
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-memos").is_ok());
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(
      berth::atomic_write(p.units_dir / "berthpanel-memos.service", "[Unit]\n")
          .is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(!report.value().lock_busy);
  CHECK(report.value().swept.size() == 1);
  CHECK(report.value().swept.front().stage == "registering");
  CHECK(called(ops, "stop:berthpanel-memos"));
  CHECK(called(ops, "stop:berthpanel-build-berthpanel-memos"));
  CHECK(!std::filesystem::exists(p.services_dir / "berthpanel-memos"));
  CHECK(!std::filesystem::exists(p.units_dir / "berthpanel-memos.service"));
  CHECK(berth::journal_load(p).value().empty());
}

TEST(sweep_removes_the_enable_symlink_left_by_an_interrupted_deploy) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p,
                              stale_entry(p, "berthpanel-memos", "registering"))
            .is_ok());
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-memos").is_ok());
  // start_service ran `enable --now`: a unit file AND a wants symlink.
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(
      berth::atomic_write(p.units_dir / "berthpanel-memos.service", "[Unit]\n")
          .is_ok());
  const std::filesystem::path wants =
      p.units_dir / "default.target.wants" / "berthpanel-memos.service";
  CHECK(berth::ensure_dir(wants.parent_path()).is_ok());
  std::error_code ec;
  std::filesystem::create_symlink(p.units_dir / "berthpanel-memos.service",
                                  wants, ec);
  CHECK(!ec);

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.size() == 1);
  CHECK(!std::filesystem::exists(p.units_dir / "berthpanel-memos.service"));
  // The symlink itself must be gone, dangling or not.
  CHECK(!std::filesystem::is_symlink(wants));
  CHECK(std::filesystem::symlink_status(wants).type() ==
        std::filesystem::file_type::not_found);
}

TEST(sweep_refuses_a_work_dir_outside_the_services_layout) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  berth::JournalEntry hostile = stale_entry(p, "berthpanel-memos", "cloning");
  hostile.work_dir = "/etc";  // hand-edited journal (§11 trap)
  CHECK(berth::journal_upsert(p, hostile).is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.empty());
  CHECK(report.value().failed.empty());
  CHECK(report.value().skipped_outside.size() == 1);
  CHECK(report.value().skipped_outside.front() == "berthpanel-memos");
  CHECK(ops.calls.empty());  // nothing was stopped, nothing was deleted
  CHECK(berth::journal_load(p).value().size() == 1);  // entry kept, loud
  CHECK(std::filesystem::exists("/etc"));             // untouched, obviously
}

TEST(sweep_refuses_a_work_dir_equal_to_the_services_dir) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.services_dir).is_ok());
  berth::JournalEntry greedy = stale_entry(p, "berthpanel-memos", "cloning");
  greedy.work_dir = p.services_dir.string();  // containment must be strict
  CHECK(berth::journal_upsert(p, greedy).is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.empty());
  CHECK(report.value().skipped_outside.size() == 1);
  CHECK(ops.calls.empty());
  CHECK(std::filesystem::exists(p.services_dir));
}

TEST(sweep_refuses_a_work_dir_that_symlinks_out_of_the_layout) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  // Lexically this work_dir looks perfect; the symlink is the attack.
  const std::filesystem::path escape = tmp.path() / "elsewhere";
  CHECK(berth::ensure_dir(escape).is_ok());
  CHECK(berth::atomic_write(escape / "data", "keep me").is_ok());
  CHECK(berth::ensure_dir(p.services_dir).is_ok());
  std::error_code ec;
  std::filesystem::create_symlink(escape, p.services_dir / "berthpanel-memos",
                                  ec);
  CHECK(!ec);
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-memos", "cloning"))
            .is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.empty());
  CHECK(report.value().skipped_outside.size() == 1);
  CHECK(ops.calls.empty());
  // Neither the symlink nor its target was touched.
  CHECK(std::filesystem::is_symlink(p.services_dir / "berthpanel-memos"));
  CHECK(std::filesystem::exists(escape / "data"));
}

TEST(sweep_fails_an_entry_whose_work_dir_is_inside_but_mismatched) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.services_dir).is_ok());
  berth::JournalEntry shifted = stale_entry(p, "berthpanel-memos", "cloning");
  // Inside the layout, but not THIS service's directory.
  shifted.work_dir = (p.services_dir / "berthpanel-other").string();
  CHECK(berth::journal_upsert(p, shifted).is_ok());
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-other").is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.empty());
  CHECK(report.value().skipped_outside.empty());
  CHECK(report.value().failed.size() == 1);
  CHECK(report.value().failed.front().second.find("is not") !=
        std::string::npos);
  CHECK(ops.calls.empty());
  CHECK(std::filesystem::exists(p.services_dir / "berthpanel-other"));
}

TEST(sweep_drops_a_stale_entry_when_the_service_is_in_the_registry) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-memos", "cloning"))
            .is_ok());
  // Leftovers the guard must NOT delete: a registered service owns them.
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-memos").is_ok());
  CHECK(
      berth::atomic_write(p.services_dir / "berthpanel-memos" / "data", "live")
          .is_ok());
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(
      berth::atomic_write(p.units_dir / "berthpanel-memos.service", "[Unit]\n")
          .is_ok());
  berth::ServiceRecord record;
  record.service_name = "berthpanel-memos";
  record.port = 8234;
  CHECK(berth::registry_register(p, record).is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(!report.value().lock_busy);
  CHECK(report.value().swept.empty());
  CHECK(report.value().failed.empty());
  CHECK(report.value().skipped_registry.size() == 1);
  CHECK(report.value().skipped_registry.front() == "berthpanel-memos");
  CHECK(ops.calls.empty());  // a live service is never stopped by cleanup
  CHECK(std::filesystem::exists(p.services_dir / "berthpanel-memos" / "data"));
  CHECK(std::filesystem::exists(p.units_dir / "berthpanel-memos.service"));
  // The entry was the stale part: dropped loudly, nothing else touched.
  CHECK(berth::journal_load(p).value().empty());
  const auto logged = berth::read_file(p.deploy_log);
  CHECK(logged.is_ok());
  CHECK(logged.value().find("is in the registry") != std::string::npos);
}

TEST(sweep_refuses_to_run_when_the_registry_is_unreadable) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, stale_entry(p, "berthpanel-memos", "cloning"))
            .is_ok());
  CHECK(berth::ensure_dir(p.services_dir / "berthpanel-memos").is_ok());
  CHECK(berth::ensure_dir(p.units_dir).is_ok());
  CHECK(
      berth::atomic_write(p.units_dir / "berthpanel-memos.service", "[Unit]\n")
          .is_ok());
  // Without a readable registry no entry can be proven unregistered, so
  // the sweep must refuse rather than delete unguarded (§11: a corrupt
  // state never reads as empty).
  CHECK(
      berth::atomic_write(p.state_json, "{oops", berth::kPermPrivate).is_ok());

  FakeOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_err());
  CHECK(ops.calls.empty());
  CHECK(std::filesystem::exists(p.services_dir / "berthpanel-memos"));
  CHECK(std::filesystem::exists(p.units_dir / "berthpanel-memos.service"));
  CHECK(berth::journal_load(p).value().size() == 1);
}

// --- start_cmd argv: the exec interpreter must be absolute ----------------

TEST(deploy_resolves_a_bare_start_interpreter_through_path) {
  const TempDir tmp;
  put_executable(tmp.path() / "bin", "bun");
  Recipe r = prebuilt_recipe();
  r.binary_name = "server/server.js";
  r.start_cmd = "bun {project}/server/server.js --host=127.0.0.1 --port={port}";

  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set((tmp.path() / "bin").string());

  std::string binary;
  const auto argv =
      berth::resolve_start_argv(r, tmp.path() / "wd", 3001, binary);
  CHECK(argv.is_ok());
  // The interpreter becomes its absolute PATH hit; the unit then does not
  // depend on systemd's PATH (which lacks ~/.bun/bin) to start.
  CHECK(binary == (tmp.path() / "bin" / "bun").string());
  CHECK(argv.value().size() == 3);
  CHECK(argv.value()[0] ==
        (tmp.path() / "wd" / "server" / "server.js").string());
  CHECK(argv.value()[1] == "--host=127.0.0.1");
  CHECK(argv.value()[2] == "--port=3001");
}

TEST(deploy_fails_loudly_when_the_start_interpreter_is_absent) {
  const TempDir tmp;
  Recipe r = prebuilt_recipe();
  r.binary_name = "server/server.js";
  r.start_cmd =
      "definitely-absent-bun {project}/server/server.js --host=127.0.0.1 "
      "--port {port}";

  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set(tmp.path().string());  // an empty directory: nothing resolves

  std::string binary;
  const auto argv = berth::resolve_start_argv(r, tmp.path(), 3001, binary);
  CHECK(argv.is_err());
  CHECK(argv.error().ctx().find("definitely-absent-bun") != std::string::npos);
}

TEST(deploy_keeps_an_absolute_self_without_consulting_path) {
  const TempDir tmp;
  const Recipe r = prebuilt_recipe();  // start_cmd uses the absolute {self}
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.unset();  // an already-absolute program must not need PATH

  std::string binary;
  const auto argv = berth::resolve_start_argv(r, tmp.path(), 8234, binary);
  CHECK(argv.is_ok());
  CHECK(binary == (tmp.path() / "memos").string());
  CHECK(argv.value().front() == "--addr");
}