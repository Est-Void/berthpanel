#include "berth/secret_ops.h"

#include <filesystem>
#include <string>
#include <vector>

#include "berth/journal.h"
#include "berth/secrets.h"
#include "core/atomic.h"
#include "core/names.h"
#include "core/state.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::Result;
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
  p.crash_log = p.state_dir / "crash.log";
  return p;
}

ServiceRecord record(const std::string& service_name) {
  ServiceRecord r;
  r.service_name = service_name;
  r.display_name = service_name;
  r.repo = "owner/repo";
  r.commit = std::string(40, 'a');
  r.port = 8234;
  return r;
}

}

TEST(secret_rotation_writes_then_restarts_without_a_redeploy) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, record("berthpanel-memos")).is_ok());
  CHECK(berth::write_secrets(p, "berthpanel-memos", {{"A", "one"}}).is_ok());

  std::vector<std::string> restarted;
  const auto restart = [&restarted](const std::string& name) -> Result<void> {
    restarted.push_back(name);
    return {};
  };
  CHECK(berth::rotate_secret(p, "berthpanel-memos", "A", "two", restart)
            .is_ok());
  CHECK((restarted == std::vector<std::string>{"berthpanel-memos"}));

  const auto body =
      berth::read_file(p.secrets_dir / "berthpanel-memos.env");
  CHECK(body.is_ok());
  CHECK(body.value() == "A=\"two\"\n");
  CHECK(std::filesystem::status(p.secrets_dir / "berthpanel-memos.env")
            .permissions() == berth::kPermPrivate);

  // Rotation is NOT a redeploy: no unit file, no journal entry, registry kept.
  CHECK(!std::filesystem::exists(p.units_dir / "berthpanel-memos.service"));
  CHECK(berth::journal_load(p).value().empty());
  CHECK(berth::registry_get(p, "berthpanel-memos").value().has_value());
}

TEST(secret_rotation_refuses_an_unregistered_service) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  bool restarted = false;
  const auto restart = [&restarted](const std::string&) -> Result<void> {
    restarted = true;
    return {};
  };
  const auto rotated =
      berth::rotate_secret(p, "berthpanel-memos", "A", "two", restart);
  CHECK(rotated.is_err());
  CHECK(!restarted);
  CHECK(!std::filesystem::exists(p.secrets_dir / "berthpanel-memos.env"));
}

TEST(secret_rotation_rejects_an_invalid_service_name) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  bool restarted = false;
  const auto rotated =
      berth::rotate_secret(p, "memos", "A", "two",
                           [&restarted](const std::string&) -> Result<void> {
                             restarted = true;
                             return {};
                           });
  CHECK(rotated.is_err());
  CHECK(!restarted);
}

TEST(secret_rotation_never_leaks_the_value_into_the_logs) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::registry_register(p, record("berthpanel-memos")).is_ok());
  CHECK(berth::write_secrets(p, "berthpanel-memos", {{"TOKEN", "old"}})
            .is_ok());
  CHECK(berth::ensure_dir(p.state_dir).is_ok());
  CHECK(berth::atomic_write(p.deploy_log, "deploy: before\n",
                            berth::kPermPrivate)
            .is_ok());
  CHECK(berth::atomic_write(p.net_log, "net: before\n", berth::kPermPrivate)
            .is_ok());
  CHECK(berth::atomic_write(p.crash_log, "crash: before\n",
                            berth::kPermPrivate)
            .is_ok());
  const std::vector<std::filesystem::path> logs = {p.deploy_log, p.net_log,
                                                   p.crash_log};

  const std::string secret = "TOP-SECRET-9f3a";
  const auto restart_ok = [](const std::string&) -> Result<void> { return {}; };
  CHECK(berth::rotate_secret(p, "berthpanel-memos", "TOKEN", secret, restart_ok)
            .is_ok());
  for (const std::filesystem::path& log : logs) {
    const auto body = berth::read_file(log);
    CHECK(body.is_ok());
    CHECK(body.value().find(secret) == std::string::npos);
  }

  // A failing restart returns the error; it must not log the value either.
  const std::string failed_secret = "TOP-SECRET-fail-77";
  const auto restart_fail = [](const std::string&) -> Result<void> {
    return berth::Error("restart failed");
  };
  const auto failed = berth::rotate_secret(p, "berthpanel-memos", "TOKEN",
                                           failed_secret, restart_fail);
  CHECK(failed.is_err());
  CHECK(failed.error().ctx().find(failed_secret) == std::string::npos);
  for (const std::filesystem::path& log : logs) {
    const auto body = berth::read_file(log);
    CHECK(body.is_ok());
    CHECK(body.value().find(failed_secret) == std::string::npos);
  }
}
