#include "core/atomic.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <optional>
#include <string>

#include "util/errno.h"
#include "util/fd.h"
#include "util/fs.h"

namespace berth {

namespace {

constexpr int kMaxTempAttempts = 8;
constexpr int kMaxBackupAttempts = 64;

std::string temp_name(const std::filesystem::path& dest, const int attempt) {
  return dest.string() + ".tmp." + std::to_string(::getpid()) + "." +
         std::to_string(attempt);
}

std::string backup_name(const std::filesystem::path& original,
                        const int index) {
  const std::string suffix = index == 0 ? std::string(".corrupt")
                                        : ".corrupt." + std::to_string(index);
  return original.string() + suffix;
}

// Only an O_EXCL name collision is worth retrying; every other failure is
// reported as-is so the caller never spins on a real problem.
enum class TempStatus { kWritten, kNameTaken, kFailed };

struct TempOutcome {
  TempStatus status = TempStatus::kFailed;
  std::optional<Error> error;
};

Result<void> write_temp_body(const int fd, const std::string_view data,
                             const std::filesystem::perms perms) {
  const Result<void> written = write_all(fd, data);
  if (written.is_err())
    return written;
  if (::fchmod(fd, static_cast<mode_t>(perms)) != 0) {
    return Error("fchmod: " + errno_text(errno));
  }
  if (::fsync(fd) != 0) {
    return Error("fsync: " + errno_text(errno));
  }
  return {};
}

TempOutcome commit_temp(const std::filesystem::path& dest,
                        const std::string& tmp, FdGuard fd,
                        const std::string_view data,
                        const std::filesystem::perms perms) {
  const Result<void> body = write_temp_body(fd.get(), data, perms);
  fd.close_now();
  if (body.is_err()) {
    ::unlink(tmp.c_str());
    return {TempStatus::kFailed,
            with_context(std::move(body).error(), "atomic_write: " + tmp)};
  }
  if (::rename(tmp.c_str(), dest.c_str()) != 0) {
    const std::string why = errno_text(errno);
    ::unlink(tmp.c_str());
    return {TempStatus::kFailed, Error("atomic_write: rename " + tmp + " -> " +
                                       dest.string() + ": " + why)};
  }
  return {TempStatus::kWritten, std::nullopt};
}

TempOutcome write_once(const std::filesystem::path& dest,
                       const std::string_view data,
                       const std::filesystem::perms perms, const int attempt) {
  const std::string tmp = temp_name(dest, attempt);
  const int raw = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                         static_cast<mode_t>(perms));
  if (raw < 0) {
    const int saved = errno;
    if (saved == EEXIST)
      return {TempStatus::kNameTaken, std::nullopt};
    return {TempStatus::kFailed,
            Error("atomic_write: open " + tmp + ": " + errno_text(saved))};
  }
  return commit_temp(dest, tmp, FdGuard(raw), data, perms);
}

Result<std::filesystem::path> free_backup_name(const std::string& candidate) {
  std::error_code ec;
  if (std::filesystem::exists(candidate, ec)) {
    return std::filesystem::path();
  }
  if (ec) {
    return Error("backup_unique: stat " + candidate + ": " + ec.message());
  }
  return std::filesystem::path(candidate);
}

Result<std::filesystem::path> next_backup_name(
    const std::filesystem::path& original) {
  for (int i = 0; i < kMaxBackupAttempts; ++i) {
    const Result<std::filesystem::path> candidate =
        free_backup_name(backup_name(original, i));
    if (candidate.is_err())
      return std::move(candidate).error();
    if (!candidate.value().empty())
      return candidate;
  }
  return Error("backup_unique: no free backup name for " + original.string());
}

}
Result<void> atomic_write(const std::filesystem::path& dest,
                          const std::string_view data,
                          const std::filesystem::perms perms) {
  for (int attempt = 0; attempt < kMaxTempAttempts; ++attempt) {
    const TempOutcome outcome = write_once(dest, data, perms, attempt);
    if (outcome.status == TempStatus::kWritten)
      return {};
    if (outcome.error.has_value())
      return *outcome.error;
  }
  return Error("atomic_write: temp-name collision at " + dest.string() +
               " after " + std::to_string(kMaxTempAttempts) + " attempts");
}

Result<std::filesystem::path> backup_unique(
    const std::filesystem::path& original) {
  const Result<std::string> content = read_file(original);
  if (content.is_err()) {
    return with_context(std::move(content).error(),
                        "backup_unique: reading " + original.string());
  }
  const Result<std::filesystem::path> candidate = next_backup_name(original);
  if (candidate.is_err())
    return std::move(candidate).error();

  const Result<void> written =
      atomic_write(candidate.value(), content.value(), kPermPrivate);
  if (written.is_err()) {
    return with_context(std::move(written).error(),
                        "backup_unique: writing " + candidate.value().string());
  }
  return candidate;
}

}
