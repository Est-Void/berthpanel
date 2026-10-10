// Tests for backup.h (stage 5B.4): the ustar writer, a backup->wipe->restore
// round trip on temp, secrets staying 0600, and the restore path's loud
// refusal of traversal, absolute paths, escaping symlinks and unexpected
// entries. Nothing here touches systemd or the network.

#include "berth/backup.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "berth/net/tar.h"
#include "berth/secrets.h"
#include "core/atomic.h"
#include "core/names.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::TarFile;

namespace {

berth::Paths backup_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.data_dir = root / "data" / "berthpanel";
  p.services_dir = p.data_dir / "services";
  p.config_dir = root / "config" / "berthpanel";
  p.secrets_dir = p.state_dir / "secrets";
  p.state_json = p.state_dir / "state.json";
  return p;
}

void write_file(const std::filesystem::path& path, const std::string& body,
                std::filesystem::perms perms) {
  std::filesystem::create_directories(path.parent_path());
  const auto made = berth::atomic_write(path, body, perms);
  CHECK(made.is_ok());
}

// Helpers so every field is initialized (a designated initializer that skips
// a member trips -Wmissing-field-initializers under -Wextra).
TarFile t_file(std::string name, std::string data,
               const std::uint32_t mode = 0644) {
  return TarFile{.name = std::move(name),
                 .typeflag = '0',
                 .mode = mode,
                 .data = std::move(data),
                 .link_target = ""};
}

TarFile t_dir(std::string name) {
  return TarFile{.name = std::move(name),
                 .typeflag = '5',
                 .mode = 0755,
                 .data = "",
                 .link_target = ""};
}

TarFile t_symlink(std::string name, std::string target) {
  return TarFile{.name = std::move(name),
                 .typeflag = '2',
                 .mode = 0777,
                 .data = "",
                 .link_target = std::move(target)};
}

}  // namespace

TEST(backup_tar_create_lists_back) {
  const std::string bytes =
      berth::tar_create({t_file("data/hello.txt", "hi"), t_dir("data/dir/"),
                         t_symlink("data/link", "hello.txt")})
          .value();
  const auto entries = berth::tar_list(bytes);
  CHECK(entries.is_ok());
  CHECK(entries.value().size() == 3);
  CHECK(entries.value()[0].name == "data/hello.txt");
  CHECK(entries.value()[0].size == 2);
  CHECK(entries.value()[0].typeflag == '0');
  CHECK(entries.value()[1].typeflag == '5');
  CHECK(entries.value()[2].typeflag == '2');
  CHECK(entries.value()[2].link_target == "hello.txt");
}

TEST(backup_round_trip_preserves_data_and_secrets) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-memos";

  const std::filesystem::path data = p.services_dir / service;
  write_file(data / "notes.db", "the database", berth::kPermPrivate);
  write_file(data / "sub" / "upload.bin", "payload", berth::kPermShared);
  write_file(data / "run.sh", "#!/bin/sh\n",
             std::filesystem::perms::owner_all |
                 std::filesystem::perms::group_read |
                 std::filesystem::perms::group_exec |
                 std::filesystem::perms::others_read |
                 std::filesystem::perms::others_exec);
  CHECK(std::filesystem::create_directories(data / "empty"));

  CHECK(berth::write_secrets(p, service, {{"SECRET_TOKEN", "s3cr3t"}}).is_ok());

  const std::filesystem::path archive = tmp.path() / "out" / "memos.tar";
  const auto report = berth::backup_to_archive(p, service, archive, nullptr);
  CHECK(report.is_ok());
  CHECK(report.value().had_secrets);
  CHECK(std::filesystem::exists(archive));
  // The archive carries secrets, so it is 0600.
  CHECK(std::filesystem::status(archive).permissions() == berth::kPermPrivate);
  // Atomic write: no temp leftover in the output directory.
  std::size_t out_files = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(archive.parent_path())) {
    (void)entry;
    ++out_files;
  }
  CHECK(out_files == 1);

  // Wipe the deployed state, then restore it.
  CHECK(berth::remove_tree(data).is_ok());
  CHECK(berth::remove_tree(p.secrets_dir).is_ok());

  const auto bytes = berth::read_file(archive);
  CHECK(bytes.is_ok());
  const auto restored =
      berth::restore_from_archive(p, service, bytes.value(), false);
  CHECK(restored.is_ok());
  CHECK(restored.value().restored_data);
  CHECK(restored.value().restored_secrets);

  const auto notes = berth::read_file(data / "notes.db");
  CHECK(notes.is_ok());
  CHECK(notes.value() == "the database");
  const auto upload = berth::read_file(data / "sub" / "upload.bin");
  CHECK(upload.is_ok());
  CHECK(upload.value() == "payload");
  CHECK(std::filesystem::is_directory(data / "empty"));
  // The executable bit survived the round trip.
  CHECK((std::filesystem::status(data / "run.sh").permissions() &
         std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
  // Secrets are restored 0600.
  const std::filesystem::path secret = p.secrets_dir / (service + ".env");
  CHECK(std::filesystem::exists(secret));
  CHECK(std::filesystem::status(secret).permissions() == berth::kPermPrivate);
  // No staging dir survives.
  CHECK(!std::filesystem::exists(p.services_dir /
                                 (service + ".restore-staging")));
}

TEST(backup_restore_refuses_traversal_and_absolute_paths) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-x";

  const std::string traversal =
      berth::tar_create({t_file("data/../../evil.txt", "x")}).value();
  CHECK(berth::restore_from_archive(p, service, traversal, false).is_err());

  const std::string absolute =
      berth::tar_create({t_file("/etc/evil", "x")}).value();
  CHECK(berth::restore_from_archive(p, service, absolute, false).is_err());
}

TEST(backup_restore_refuses_an_escaping_symlink) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-x";

  const std::string symlink =
      berth::tar_create({t_symlink("data/link", "/etc/passwd")}).value();
  CHECK(berth::restore_from_archive(p, service, symlink, false).is_err());

  const std::string relative =
      berth::tar_create({t_symlink("data/link", "../../outside")}).value();
  CHECK(berth::restore_from_archive(p, service, relative, false).is_err());
}

TEST(backup_restore_refuses_unexpected_entries) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-x";

  const std::string stray =
      berth::tar_create({t_file("evil.txt", "x")}).value();
  CHECK(berth::restore_from_archive(p, service, stray, false).is_err());
}

TEST(backup_restore_over_existing_data_needs_force) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-memos";
  CHECK(berth::write_secrets(p, service, {{"K", "v"}}).is_ok());
  write_file(p.services_dir / service / "notes.db", "old", berth::kPermPrivate);
  CHECK(berth::backup_to_archive(p, service, tmp.path() / "b.tar", nullptr)
            .is_ok());
  const auto bytes = berth::read_file(tmp.path() / "b.tar");
  CHECK(bytes.is_ok());

  // Existing data and no --force: refuse, touching nothing.
  CHECK(berth::restore_from_archive(p, service, bytes.value(), false).is_err());
  const auto still = berth::read_file(p.services_dir / service / "notes.db");
  CHECK(still.is_ok());
  CHECK(still.value() == "old");

  // With --force it replaces.
  CHECK(berth::restore_from_archive(p, service, bytes.value(), true).is_ok());
  const auto replaced = berth::read_file(p.services_dir / service / "notes.db");
  CHECK(replaced.is_ok());
  CHECK(replaced.value() == "old");
}

TEST(backup_empty_service_is_a_loud_error) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const auto report = berth::backup_to_archive(p, "berthpanel-missing",
                                               tmp.path() / "x.tar", nullptr);
  CHECK(report.is_err());
  CHECK(berth::backup_to_archive(p, "bad/name", tmp.path() / "x.tar", nullptr)
            .is_err());
  CHECK(!std::filesystem::exists(tmp.path() / "x.tar"));
}

TEST(backup_refuses_a_bad_output_path_without_partial_file) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-memos";
  CHECK(berth::write_secrets(p, service, {{"K", "v"}}).is_ok());

  // The parent is a regular FILE, so the directory cannot be created and no
  // archive may appear.
  write_file(tmp.path() / "blocker", "not a dir", berth::kPermPrivate);
  const std::filesystem::path output = tmp.path() / "blocker" / "x.tar";
  CHECK(berth::backup_to_archive(p, service, output, nullptr).is_err());
  CHECK(!std::filesystem::exists(output));
}

TEST(backup_restore_secrets_only_archive) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  const std::string service = "berthpanel-secret-only";
  CHECK(berth::write_secrets(p, service, {{"K", "v"}}).is_ok());

  const std::filesystem::path archive = tmp.path() / "s.tar";
  CHECK(berth::backup_to_archive(p, service, archive, nullptr).is_ok());
  CHECK(berth::remove_tree(p.secrets_dir).is_ok());

  const auto bytes = berth::read_file(archive);
  CHECK(bytes.is_ok());
  const auto restored =
      berth::restore_from_archive(p, service, bytes.value(), false);
  CHECK(restored.is_ok());
  CHECK(!restored.value().restored_data);  // no data/ in the archive
  CHECK(restored.value().restored_secrets);
  CHECK(std::filesystem::exists(p.secrets_dir / (service + ".env")));
}

TEST(backup_default_path_is_deterministic) {
  const TempDir tmp;
  const berth::Paths p = backup_paths(tmp.path());
  CHECK(berth::default_backup_path(p, "berthpanel-memos") ==
        p.state_dir / "backups" / "berthpanel-memos.tar");
}
