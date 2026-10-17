#include "berth/stacks.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "berth/catalog.h"
#include "berth/deploy.h"
#include "berth/journal.h"
#include "berth/lock.h"
#include "berth/recipes.h"
#include "core/atomic.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "stack_kill_child.h"
#include "test_env.h"
#include "util/fs.h"
#include "util/subprocess.h"

using berth::Recipe;
using berth::Result;
using berth::Stack;
using berth::StackMember;

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

berth::JournalEntry group_entry(const berth::Paths& p, const std::string& group,
                                const std::string& member,
                                const std::string& stage) {
  berth::JournalEntry e;
  e.service_name = member;
  e.work_dir = (p.services_dir / member).string();
  e.repo = "owner/repo";
  e.commit = std::string(40, 'a');
  e.stage = stage;
  e.stack = group;
  e.started_at_unix = 1;
  return e;
}

Recipe prebuilt_member(const std::string& service_name,
                       const std::string& name) {
  Recipe r;
  r.owner = "owner";
  r.name = name;
  r.commit = std::string(40, 'a');
  r.language = "prebuilt";
  r.service_name = service_name;
  r.display_name = name;
  r.description = name;
  r.prebuilt = berth::PrebuiltBinary{
      .url = "https://github.com/owner/" + name +
             "/releases/download/v1/x.tar.gz",
      .sha256 = std::string(64, 'b'),
      .member = name};
  r.binary_name = name;
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port}";
  r.tools = {};
  r.res = {"256M", 100};
  return r;
}

// Members are listed app-first on purpose: the deploy must follow the
// after/wants edge and start db before app.
Stack demo_stack() {
  Stack s;
  s.service_name = "berthpanel-demo";
  s.display_name = "Demo";
  s.description = "stack";
  StackMember app;
  app.name = "app";
  app.recipe = prebuilt_member("berthpanel-demo-app", "gitea");
  app.after = {"db"};
  app.wants = {"db"};
  StackMember db;
  db.name = "db";
  db.recipe = prebuilt_member("berthpanel-demo-db", "postgres");
  s.members = {app, db};
  return s;
}

class FakeStackOps final : public berth::DeployOps {
 public:
  std::vector<std::string> calls;
  std::string fail_build_service;
  bool fail_stop = false;

  Result<void> fetch_source(const Recipe& recipe,
                            const std::filesystem::path& work_dir,
                            const berth::Paths& paths,
                            const berth::DeployOptions& options) override {
    static_cast<void>(work_dir);
    static_cast<void>(paths);
    static_cast<void>(options);
    calls.push_back("fetch:" + recipe.service_name);
    return {};
  }
  Result<void> prefetch(const Recipe& recipe,
                        const std::filesystem::path& work_dir,
                        const berth::Paths& paths,
                        const berth::DeployOptions& options) override {
    static_cast<void>(work_dir);
    static_cast<void>(paths);
    static_cast<void>(options);
    calls.push_back("prefetch:" + recipe.service_name);
    return {};
  }
  Result<void> build(const Recipe& recipe,
                     const std::filesystem::path& work_dir, std::uint16_t port,
                     const berth::Paths& paths,
                     const berth::DeployOptions& options) override {
    static_cast<void>(work_dir);
    static_cast<void>(port);
    static_cast<void>(paths);
    static_cast<void>(options);
    calls.push_back("build:" + recipe.service_name);
    if (recipe.service_name == fail_build_service)
      return berth::Error("fake build failure for " + recipe.service_name);
    return {};
  }
  Result<std::filesystem::path> write_unit_file(
      const Recipe& recipe, const std::filesystem::path& work_dir,
      std::uint16_t port, const berth::Paths& paths) override {
    static_cast<void>(port);
    static_cast<void>(paths);
    calls.push_back("unit:" + recipe.service_name);
    return work_dir / (recipe.service_name + ".service");
  }
  Result<void> start_service(const std::string& service_name) override {
    calls.push_back("start:" + service_name);
    return {};
  }
  Result<void> await_running(const std::string& service_name,
                             const berth::Paths& paths,
                             std::atomic<bool>* cancel) override {
    static_cast<void>(paths);
    static_cast<void>(cancel);
    calls.push_back("await:" + service_name);
    return {};
  }
  Result<void> verify_listening(const std::string& service_name,
                                std::uint16_t port, const berth::Paths& paths,
                                std::atomic<bool>* cancel) override {
    static_cast<void>(service_name);
    static_cast<void>(port);
    static_cast<void>(paths);
    static_cast<void>(cancel);
    calls.push_back("listen:" + service_name);
    return {};
  }
  Result<void> register_service(const berth::ServiceRecord& record,
                                const berth::Paths& paths) override {
    calls.push_back("register:" + record.service_name);
    return berth::registry_register(paths, record);
  }
  Result<bool> stop_unit(const std::string& unit_stem,
                         std::atomic<bool>* cancel) override {
    static_cast<void>(cancel);
    calls.push_back("stop:" + unit_stem);
    if (fail_stop)
      return berth::Error("fake stop failure for " + unit_stem);
    return false;
  }
};

bool called(const FakeStackOps& ops, const std::string& step) {
  return std::find(ops.calls.begin(), ops.calls.end(), step) != ops.calls.end();
}

std::string self_exe() {
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return "";
  buf[n] = '\0';
  return std::string(buf);
}

}

int stack_kill_child_main(int argc, char** argv) {
  if (argc < 7)
    return 2;
  const std::vector<std::string> args(argv, argv + argc);
  berth::Paths p;
  p.deploy_lock = args[0];
  p.deploy_journal = args[1];
  p.state_dir = p.deploy_journal.parent_path();
  p.services_dir = args[2];
  p.units_dir = args[3];
  p.state_json = args[4];
  const std::string group = args[5];

  const auto lock = berth::DeployLock::acquire(p);
  if (lock.is_err())
    return 3;
  const std::int64_t now = static_cast<std::int64_t>(::time(nullptr));
  for (std::size_t i = 6; i < args.size(); ++i) {
    const std::string& member = args[i];
    berth::JournalEntry entry;
    entry.service_name = member;
    entry.work_dir = (p.services_dir / member).string();
    entry.repo = "owner/repo";
    entry.commit = std::string(40, 'a');
    entry.stage = "registering";
    entry.stack = group;
    entry.started_at_unix = now;
    if (berth::journal_upsert(p, entry).is_err())
      return 4;
    if (berth::ensure_dir(p.services_dir / member).is_err())
      return 5;
    if (berth::atomic_write(p.services_dir / member / "data", "x").is_err())
      return 6;
    if (berth::ensure_dir(p.units_dir).is_err())
      return 7;
    if (berth::atomic_write(p.units_dir / (member + ".service"), "[Unit]\n")
            .is_err())
      return 8;
  }
  ::pause();
  return 0;
}

TEST(stack_deploy_runs_members_in_dependency_order_and_registers_both) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeStackOps ops;
  berth::DeployOptions options;
  const auto outcome = berth::run_stack_deploy(p, demo_stack(), ops, options);
  CHECK(outcome.is_ok());
  CHECK(outcome.value().stack_name == "berthpanel-demo");
  CHECK(outcome.value().members.size() == 2);
  CHECK(outcome.value().members[0].service_name == "berthpanel-demo-db");
  CHECK(outcome.value().members[1].service_name == "berthpanel-demo-app");

  const std::vector<std::string> expected = {
      "fetch:berthpanel-demo-db",   "prefetch:berthpanel-demo-db",
      "build:berthpanel-demo-db",   "unit:berthpanel-demo-db",
      "start:berthpanel-demo-db",   "await:berthpanel-demo-db",
      "listen:berthpanel-demo-db",  "register:berthpanel-demo-db",
      "fetch:berthpanel-demo-app",  "prefetch:berthpanel-demo-app",
      "build:berthpanel-demo-app",  "unit:berthpanel-demo-app",
      "start:berthpanel-demo-app",  "await:berthpanel-demo-app",
      "listen:berthpanel-demo-app", "register:berthpanel-demo-app"};
  CHECK(ops.calls == expected);

  CHECK(berth::registry_get(p, "berthpanel-demo-db").value().has_value());
  CHECK(berth::registry_get(p, "berthpanel-demo-app").value().has_value());
  CHECK(berth::journal_load(p).value().empty());
}

TEST(stack_deploy_writes_every_member_entry_before_the_first_member_runs) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeStackOps ops;
  ops.fail_build_service = "berthpanel-demo-db";  // the FIRST member fails
  berth::DeployOptions options;
  const auto outcome = berth::run_stack_deploy(p, demo_stack(), ops, options);
  CHECK(outcome.is_err());

  // Both members are on the journal, each tagged with the group: the app was
  // never fetched but cleanup can still reconcile it.
  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().size() == 2);
  bool saw_db = false;
  bool saw_app = false;
  for (const berth::JournalEntry& e : journal.value()) {
    CHECK(e.stack == "berthpanel-demo");
    if (e.service_name == "berthpanel-demo-db") {
      saw_db = true;
      CHECK(e.stage == "building");
    }
    if (e.service_name == "berthpanel-demo-app") {
      saw_app = true;
      CHECK(e.stage == "cloning");
    }
  }
  CHECK(saw_db);
  CHECK(saw_app);
  CHECK(!called(ops, "fetch:berthpanel-demo-app"));
}

TEST(stack_failure_after_the_first_member_leaves_only_the_unfinished_member) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  FakeStackOps ops;
  ops.fail_build_service = "berthpanel-demo-app";  // the SECOND member fails
  berth::DeployOptions options;
  const auto outcome = berth::run_stack_deploy(p, demo_stack(), ops, options);
  CHECK(outcome.is_err());

  // db completed and was removed from the journal; app stays for cleanup.
  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().size() == 1);
  CHECK(journal.value().front().service_name == "berthpanel-demo-app");
  CHECK(berth::registry_get(p, "berthpanel-demo-db").value().has_value());

  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.size() == 1);
  CHECK(report.value().swept.front().service_name == "berthpanel-demo-app");
  // The registered member is not the cleanup's to touch.
  CHECK(berth::registry_get(p, "berthpanel-demo-db").value().has_value());
  CHECK(berth::journal_load(p).value().empty());
}

TEST(stack_sweep_registry_guard_is_per_member) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const std::string group = "berthpanel-demo";
  CHECK(berth::journal_upsert(p, group_entry(p, group, "berthpanel-demo-db",
                                             "registering"))
            .is_ok());
  CHECK(berth::journal_upsert(p, group_entry(p, group, "berthpanel-demo-app",
                                             "registering"))
            .is_ok());
  for (const char* member : {"berthpanel-demo-db", "berthpanel-demo-app"}) {
    CHECK(berth::ensure_dir(p.services_dir / member).is_ok());
    CHECK(berth::ensure_dir(p.units_dir).is_ok());
    CHECK(berth::atomic_write(p.units_dir / (std::string(member) + ".service"),
                              "[Unit]\n")
              .is_ok());
  }
  // db completed: it is in the registry, so its leftover is NOT deleted.
  berth::ServiceRecord record;
  record.service_name = "berthpanel-demo-db";
  record.port = 8234;
  CHECK(berth::registry_register(p, record).is_ok());

  FakeStackOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().swept.size() == 1);
  CHECK(report.value().swept.front().service_name == "berthpanel-demo-app");
  CHECK(report.value().swept.front().stack == group);
  CHECK(report.value().skipped_registry.size() == 1);
  CHECK(report.value().skipped_registry.front() == "berthpanel-demo-db");
  CHECK(std::filesystem::exists(p.services_dir / "berthpanel-demo-db"));
  CHECK(std::filesystem::exists(p.units_dir / "berthpanel-demo-db.service"));
  CHECK(!std::filesystem::exists(p.services_dir / "berthpanel-demo-app"));
  CHECK(berth::registry_get(p, "berthpanel-demo-db").value().has_value());
}

TEST(stack_deploy_refuses_when_a_member_stale_entry_cannot_be_cleaned) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::journal_upsert(p, group_entry(p, "berthpanel-demo",
                                             "berthpanel-demo-db", "building"))
            .is_ok());
  FakeStackOps ops;
  ops.fail_stop = true;
  berth::DeployOptions options;
  const auto outcome = berth::run_stack_deploy(p, demo_stack(), ops, options);
  CHECK(outcome.is_err());
  CHECK(outcome.error().ctx().find("refusing to start the stack") !=
        std::string::npos);
  CHECK(!called(ops, "fetch:berthpanel-demo-db"));
}

TEST(stack_interrupt_kill_reconcile_cleans_the_group_and_spares_the_registry) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  // A neighbour already registered: cleanup must not touch it.
  berth::ServiceRecord neighbour;
  neighbour.service_name = "berthpanel-neighbour";
  neighbour.port = 9999;
  CHECK(berth::registry_register(p, neighbour).is_ok());

  const std::string self = self_exe();
  CHECK(!self.empty());
  const std::vector<std::string> members = {"berthpanel-demo-db",
                                            "berthpanel-demo-app"};
  std::vector<std::string> cmd = {self,
                                  "__test-stack-kill-child",
                                  p.deploy_lock.string(),
                                  p.deploy_journal.string(),
                                  p.services_dir.string(),
                                  p.units_dir.string(),
                                  p.state_json.string(),
                                  "berthpanel-demo"};
  for (const std::string& member : members)
    cmd.push_back(member);

  // The child holds the lock and pauses; the timeout SIGKILLs it, so no
  // cleanup code runs in the child — only the kernel dropping the flock.
  const auto killed = berth::run(
      cmd, berth::RunLimits{.timeout = std::chrono::milliseconds(1000)});
  CHECK(killed.is_err());

  const auto journal = berth::journal_load(p);
  CHECK(journal.is_ok());
  CHECK(journal.value().size() == 2);
  for (const berth::JournalEntry& e : journal.value()) {
    CHECK(e.stack == "berthpanel-demo");
    CHECK(std::filesystem::exists(p.services_dir / e.service_name));
    CHECK(std::filesystem::exists(p.units_dir /
                                  (e.service_name + ".service")));
  }

  FakeStackOps ops;
  const auto report = berth::reconcile_interrupted(p, ops, nullptr);
  CHECK(report.is_ok());
  CHECK(!report.value().lock_busy);
  CHECK(report.value().swept.size() == 2);
  for (const std::string& member : members) {
    CHECK(!std::filesystem::exists(p.services_dir / member));
    CHECK(!std::filesystem::exists(p.units_dir / (member + ".service")));
  }
  CHECK(berth::journal_load(p).value().empty());
  // The registry is never the cleanup's to touch.
  CHECK(berth::registry_get(p, "berthpanel-neighbour").value().has_value());
}

TEST(builtin_pilot_stack_deploys_through_the_group_engine) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto stack = berth::builtin_stack("berthpanel-gitea-stack");
  CHECK(stack.is_ok());
  FakeStackOps ops;
  berth::DeployOptions options;
  const auto outcome = berth::run_stack_deploy(p, stack.value(), ops, options);
  CHECK(outcome.is_ok());
  CHECK(outcome.value().members.size() == 2);
  // Dependency order: the stateful member is up before the app.
  CHECK(outcome.value().members[0].service_name ==
        "berthpanel-gitea-stack-db");
  CHECK(outcome.value().members[1].service_name ==
        "berthpanel-gitea-stack-app");
  CHECK(ops.calls.front() == "fetch:berthpanel-gitea-stack-db");
  CHECK(ops.calls.back() == "register:berthpanel-gitea-stack-app");
  CHECK(berth::registry_get(p, "berthpanel-gitea-stack-db")
            .value()
            .has_value());
  CHECK(berth::registry_get(p, "berthpanel-gitea-stack-app")
            .value()
            .has_value());
  CHECK(berth::journal_load(p).value().empty());
}
