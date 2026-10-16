#pragma once

// The data dir plus the 0600 secrets file, packed into ONE uncompressed ustar
// archive written atomically at 0600 (it carries secrets). The reader is
// net/tar.cpp, so restore inherits its rules: absolute paths, "..", and
// escaping symlinks fail loudly, never skipped. A LIVE service is refused: a
// mid-write backup is an inconsistent database.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/paths.h"
#include "util/result.h"

namespace berth {

// Deterministic default (no timestamp, so the path is scriptable).
[[nodiscard]] std::filesystem::path default_backup_path(
    const Paths& paths, std::string_view service);

struct BackupReport {
  std::size_t files = 0;
  std::size_t directories = 0;
  bool had_secrets = false;
  std::uint64_t bytes = 0;
};

// Writes atomically (O_EXCL temp 0600, fsync, rename): a failure never leaves a
// partial archive at `output`.
[[nodiscard]] Result<BackupReport> backup_to_archive(
    const Paths& paths, std::string_view service,
    const std::filesystem::path& output, std::atomic<bool>* cancel);

[[nodiscard]] Result<BackupReport> backup_service(
    const Paths& paths, std::string_view service,
    const std::filesystem::path& output, std::atomic<bool>* cancel);

struct RestoreReport {
  bool restored_data = false;
  bool restored_secrets = false;
  std::size_t files = 0;
};

// Without `force` a non-empty existing data dir is a loud Err: a silent
// overwrite of user data is forbidden. Does NOT consult systemd.
[[nodiscard]] Result<RestoreReport> restore_from_archive(
    const Paths& paths, std::string_view service, std::string_view archive,
    bool force);

[[nodiscard]] Result<RestoreReport> restore_service(
    const Paths& paths, std::string_view service,
    const std::filesystem::path& archive, bool force,
    std::atomic<bool>* cancel);

// Pure. It intentionally does NOT validate names: validation is the READER's
// job, and a writer that refused a bad name would make the reader untestable.
struct TarFile {
  std::string name;
  char typeflag = '0';  // '0' file, '5' dir, '2' symlink
  std::uint32_t mode = 0644;
  std::string data;
  std::string link_target;
};

[[nodiscard]] Result<std::string> tar_create(const std::vector<TarFile>& files);

}
