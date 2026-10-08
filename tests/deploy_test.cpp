#include "berth/deploy.h"

#include <algorithm>
#include <string>
#include <vector>

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
                             std::atomic<bool>* cancel) override {
    static_cast<void>(service_name);
    static_cast<void>(cancel);
    calls.push_back("await");
    if (fail_running)
      return berth::Error("unit never became active");
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

}  // namespace

TEST(deploy_happy_path_runs_steps_in_order_and_registers) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeOps ops;
  DeployOptions options;
  const auto outcome = run_deploy(p, prebuilt_recipe(), 0, ops, options);
  CHECK(outcome.is_ok());
  const std::vector<std::string> expected = {
      "fetch", "prefetch", "build", "unit", "start", "await", "register"};
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