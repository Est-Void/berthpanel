#include "atomic.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "util/errno.h"
#include "util/fd.h"

namespace berth {

namespace {
constexpr int kMaxTempAttempts = 8;
constexpr int kMaxBackupAttempts = 64;
}  // namespace

Result<void> ensure_dir(const std::filesystem::path& dir,
                        std::filesystem::perms perms) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return Error("ensure_dir: create " + dir.string() + ": " + ec.message());
  }
  if (::chmod(dir.c_str(), static_cast<mode_t>(perms)) != 0) {
    return Error("ensure_dir: chmod " + dir.string() + ": " + errno_text(errno));
  }
  return {};
}

Result<std::string> read_file(const std::filesystem::path& path) {
  const FdGuard fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    return Error("read_file: open " + path.string() + ": " + errno_text(errno));
  }
  std::string out;
  char buf[65536];
  while (true) {
    const ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Error("read_file: read " + path.string() + ": " + errno_text(errno));
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

Result<void> atomic_write(const std::filesystem::path& dest,
                          std::string_view data,
                          std::filesystem::perms perms) {
  const std::string dest_str = dest.string();
  for (int attempt = 0; attempt < kMaxTempAttempts; ++attempt) {
    const std::string tmp = dest_str + ".tmp." + std::to_string(::getpid()) +
                            "." + std::to_string(attempt);
    FdGuard fd(::open(tmp.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                            static_cast<mode_t>(perms)));    if (fd.get() < 0) {
      if (errno == EEXIST) continue;
      return Error("atomic_write: open " + tmp + ": " + errno_text(errno));
    }

    const char* cursor = data.data();
    std::size_t left = data.size();
    bool write_failed = false;
    std::string why;
    while (left > 0) {
      const ssize_t n = ::write(fd.get(), cursor, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        why = "write: " + errno_text(errno);
        write_failed = true;
        break;
      }
      cursor += n;
      left -= static_cast<std::size_t>(n);
    }
    if (!write_failed && ::fchmod(fd.get(), static_cast<mode_t>(perms)) != 0) {
      why = "fchmod: " + errno_text(errno);
      write_failed = true;
    }
    if (!write_failed && ::fsync(fd.get()) != 0) {
      why = "fsync: " + errno_text(errno);
      write_failed = true;
    }
    fd.close_now();
    if (write_failed) {
      ::unlink(tmp.c_str());
      return Error("atomic_write: " + tmp + ": " + why);
    }
    if (::rename(tmp.c_str(), dest_str.c_str()) != 0) {
      const std::string why_rename = errno_text(errno);
      ::unlink(tmp.c_str());
      return Error("atomic_write: rename " + tmp + " -> " + dest_str + ": " +
                   why_rename);
    }
    return {};
  }
  return Error("atomic_write: temp-name collision at " + dest_str +
               " after " + std::to_string(kMaxTempAttempts) + " attempts");
}

Result<std::filesystem::path> backup_unique(const std::filesystem::path& original) {
  Result<std::string> content = read_file(original);
  if (content.is_err()) {
    return with_context(std::move(content).error(),
                        "backup_unique: reading " + original.string());
  }
  for (int i = 0; i < kMaxBackupAttempts; ++i) {
    const std::string suffix =
        i == 0 ? std::string(".corrupt") : ".corrupt." + std::to_string(i);
    const std::filesystem::path candidate = original.string() + suffix;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) continue;
    if (ec) {
      return Error("backup_unique: stat " + candidate.string() + ": " + ec.message());
    }
    Result<void> written =
        atomic_write(candidate, content.value(), kPermPrivate);
    if (written.is_err()) {
      return with_context(std::move(written).error(),
                          "backup_unique: writing " + candidate.string());
    }
    return candidate;
  }
  return Error("backup_unique: no free backup name for " + original.string());
}

}  // namespace berth
