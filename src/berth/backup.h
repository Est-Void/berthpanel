#pragma once

// src/berth/backup.h — stage 5B.4: `berthpanel backup <service>` and
// `berthpanel restore <service> <archive>`.
//
// A backup is the service's data directory plus its 0600 secrets file, packed
// into ONE uncompressed ustar archive and written atomically with 0600 (it
// carries secrets). The format is deliberately minimal: ustar only, no gzip,
// no pax; the reader is net/tar.cpp, so the RESTORE path inherits every safety
// rule that module already enforces — absolute paths, "..", and symlinks that
// escape the destination fail loudly, never skipped (§2, §11).
//
// Both directions refuse a service that is LIVE: a backup taken mid-write is
// an inconsistent database, and a restore over a running service races its
// open files. The user stops the service first; nothing here stops it for them.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/paths.h"
#include "util/result.h"

namespace berth {

// state_dir/backups/<service>.tar — a deterministic default (no timestamp, so
// the path is scriptable). The caller may pass any path.
[[nodiscard]] std::filesystem::path default_backup_path(
    const Paths& paths, std::string_view service);

struct BackupReport {
  std::size_t files = 0;        // regular files + symlinks
  std::size_t directories = 0;  // directory entries
  bool had_secrets = false;
  std::uint64_t bytes = 0;  // file payload bytes (not headers)
};

// The filesystem-only backup used by `backup_service` after the liveness
// check, and directly by tests. Writes atomically: a staged O_EXCL temp
// (0600), fsync, rename; a failure never leaves a partial archive at `output`.
[[nodiscard]] Result<BackupReport> backup_to_archive(
    const Paths& paths, std::string_view service,
    const std::filesystem::path& output, std::atomic<bool>* cancel);

// Checks the service is not active, then backup_to_archive.
[[nodiscard]] Result<BackupReport> backup_service(
    const Paths& paths, std::string_view service,
    const std::filesystem::path& output, std::atomic<bool>* cancel);

struct RestoreReport {
  bool restored_data = false;
  bool restored_secrets = false;
  std::size_t files = 0;
};

// The filesystem-only restore: validates the archive structure, extracts it
// into a staging dir under services_dir, and places the data and secrets.
// Does NOT consult systemd (restore_service owns the liveness check). Hostile
// input fails loudly. With `force`, an existing data dir is replaced; without
// it, a non-empty existing data dir is a loud Err (a silent overwrite of user
// data is forbidden, §11).
[[nodiscard]] Result<RestoreReport> restore_from_archive(
    const Paths& paths, std::string_view service, std::string_view archive,
    bool force);

// Reads the archive, refuses if the service is active, then
// restore_from_archive.
[[nodiscard]] Result<RestoreReport> restore_service(
    const Paths& paths, std::string_view service,
    const std::filesystem::path& archive, bool force,
    std::atomic<bool>* cancel);

// --- the minimal ustar writer -------------------------------------------
//
// Pure and exposed for tests, including hostile archives the reader must
// reject. It intentionally does NOT validate names: validation is the
// READER's job, and a writer that refused a bad name would make the reader's
// tests impossible to write.
struct TarFile {
  std::string name;
  char typeflag = '0';  // '0' file, '5' dir, '2' symlink
  std::uint32_t mode = 0644;
  std::string data;         // for '0'
  std::string link_target;  // for '2'
};

[[nodiscard]] Result<std::string> tar_create(const std::vector<TarFile>& files);

}  // namespace berth
