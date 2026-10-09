// src/berth/backup.cpp — see backup.h for the contract.

#include "backup.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/atomic.h"
#include "core/names.h"
#include "net/tar.h"
#include "units.h"
#include "util/errno.h"
#include "util/fd.h"
#include "util/fs.h"

namespace berth {

namespace {

constexpr std::size_t kBlock = 512;
constexpr int kMaxTempAttempts = 8;

std::string octal_digits(std::uint64_t value) {
  std::string digits;
  do {
    digits.push_back(static_cast<char>('0' + (value & 7)));
    value >>= 3;
  } while (value != 0);
  std::reverse(digits.begin(), digits.end());
  return digits;
}

// Writes a NUL-terminated octal field of `width` bytes at `off`. False when
// the value does not fit, which fails the whole archive loudly rather than
// silently truncating (the §11 truncated-archive trap).
bool put_octal(std::string* block, const std::size_t off,
               const std::size_t width, const std::uint64_t value) {
  const std::string digits = octal_digits(value);
  if (digits.size() > width - 1)
    return false;
  block->replace(off + (width - 1 - digits.size()), digits.size(), digits);
  return true;
}

std::string checksum_field(const unsigned int sum) {
  std::string field(8, '\0');
  const std::string digits = octal_digits(sum);
  field.replace(6 - digits.size(), digits.size(), digits);
  field[7] = ' ';
  return field;
}

// One 512-byte ustar header. `link` is used only for typeflag '2'.
Result<std::string> build_header(const std::string& name,
                                 const std::uint32_t mode, const char typeflag,
                                 const std::uint64_t size,
                                 const std::string& link) {
  if (name.empty())
    return Error("backup: empty entry name");
  if (link.size() > 100)
    return Error("backup: link target too long for ustar");

  std::string hname = name;
  std::string prefix;
  if (name.size() > 100) {
    std::size_t split = std::string::npos;
    for (std::size_t i = name.size(); i-- > 0;) {
      if (name[i] != '/')
        continue;
      const std::size_t pre = i;
      const std::size_t suf = name.size() - i - 1;
      if (pre <= 155 && suf > 0 && suf <= 100) {
        split = i;
        break;
      }
    }
    if (split == std::string::npos)
      return Error("backup: entry name too long for ustar: " + name);
    hname = name.substr(split + 1);
    prefix = name.substr(0, split);
  }

  std::string block(kBlock, '\0');
  block.replace(0, hname.size(), hname);
  if (!put_octal(&block, 100, 8, mode))
    return Error("backup: mode overflow");
  if (!put_octal(&block, 108, 8, 0) || !put_octal(&block, 116, 8, 0))
    return Error("backup: uid/gid overflow");
  if (!put_octal(&block, 124, 12, size))
    return Error("backup: entry too large for ustar: " + name);
  if (!put_octal(&block, 136, 12, 0))
    return Error("backup: mtime overflow");
  block.replace(148, 8, "        ");  // checksum field spaces during the sum
  block[156] = typeflag;
  block.replace(157, link.size(), link);
  block.replace(257, 6, std::string("ustar\0", 6));
  block.replace(263, 2, "00");
  if (!prefix.empty())
    block.replace(345, prefix.size(), prefix);

  unsigned int sum = 0;
  for (const char c : block) sum += static_cast<unsigned char>(c);
  block.replace(148, 8, checksum_field(sum));
  return block;
}

struct Entry {
  std::string name;
  char typeflag = '0';
  std::uint32_t mode = 0644;
  std::filesystem::path source;  // '0' only
  std::string link_target;       // '2' only
  std::uint64_t size = 0;
};

// Full-field construction so no aggregate initializer skips a member (which
// trips -Wmissing-field-initializers under -Wextra).
Entry make_entry(std::string name, const char typeflag,
                 const std::uint32_t mode) {
  Entry entry;
  entry.name = std::move(name);
  entry.typeflag = typeflag;
  entry.mode = mode;
  return entry;
}

Result<void> collect_dir(const std::filesystem::path& data_dir,
                         std::vector<Entry>* out, BackupReport* report) {
  out->push_back(make_entry("data/", '5', 0755));
  ++report->directories;

  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      data_dir, std::filesystem::directory_options::none, ec);
  if (ec)
    return Error("backup: walking " + data_dir.string() + ": " + ec.message());
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec)
      return Error("backup: walking " + data_dir.string() + ": " +
                   ec.message());
    const std::filesystem::path& path = it->path();
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec)
      return Error("backup: stat " + path.string() + ": " + ec.message());
    const std::string rel = path.lexically_relative(data_dir).generic_string();
    if (rel.empty() || rel == ".")
      continue;

    if (std::filesystem::is_symlink(status)) {
      const std::filesystem::path target =
          std::filesystem::read_symlink(path, ec);
      if (ec)
        return Error("backup: readlink " + path.string() + ": " + ec.message());
      Entry entry = make_entry("data/" + rel, '2', 0777);
      entry.link_target = target.string();
      out->push_back(std::move(entry));
      ++report->files;
    } else if (std::filesystem::is_directory(status)) {
      out->push_back(make_entry("data/" + rel + "/", '5', 0755));
      ++report->directories;
    } else if (std::filesystem::is_regular_file(status)) {
      const std::uint64_t size = std::filesystem::file_size(path, ec);
      if (ec)
        return Error("backup: size " + path.string() + ": " + ec.message());
      const bool executable =
          (status.permissions() & std::filesystem::perms::owner_exec) !=
          std::filesystem::perms::none;
      Entry entry = make_entry("data/" + rel, '0', executable ? 0755u : 0644u);
      entry.source = path;
      entry.size = size;
      out->push_back(std::move(entry));
      ++report->files;
      report->bytes += size;
    } else {
      return Error("backup: unsupported file type: " + path.string());
    }
  }
  return {};
}

Result<void> copy_file(const int fd, const std::filesystem::path& source,
                       const std::uint64_t size) {
  const FdGuard in(::open(source.c_str(), O_RDONLY | O_CLOEXEC));
  if (in.get() < 0)
    return Error("backup: open " + source.string() + ": " + errno_text(errno));
  std::array<char, 65536> buffer{};
  std::uint64_t total = 0;
  while (total < size) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), size - total));
    const ssize_t n = ::read(in.get(), buffer.data(), want);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return Error("backup: read " + source.string() + ": " +
                   errno_text(errno));
    }
    if (n == 0)
      break;  // shrank under us; caught below
    const Result<void> wrote = write_all(
        fd, std::string_view(buffer.data(), static_cast<std::size_t>(n)));
    if (wrote.is_err())
      return std::move(wrote).error();
    total += static_cast<std::uint64_t>(n);
  }
  if (total != size) {
    return Error("backup: " + source.string() +
                 " changed while it was being archived");
  }
  return {};
}

Result<void> write_body(const int fd, const std::vector<Entry>& entries) {
  for (const Entry& entry : entries) {
    Result<std::string> header = build_header(
        entry.name, entry.mode, entry.typeflag, entry.size, entry.link_target);
    if (header.is_err())
      return std::move(header).error();
    const Result<void> wrote = write_all(fd, header.value());
    if (wrote.is_err())
      return std::move(wrote).error();

    if (entry.typeflag == '0') {
      const Result<void> copied = copy_file(fd, entry.source, entry.size);
      if (copied.is_err())
        return std::move(copied).error();
      const std::size_t pad =
          static_cast<std::size_t>((kBlock - (entry.size % kBlock)) % kBlock);
      if (pad > 0) {
        const Result<void> padded = write_all(fd, std::string(pad, '\0'));
        if (padded.is_err())
          return std::move(padded).error();
      }
    }
  }
  return write_all(fd, std::string(2 * kBlock, '\0'));
}

Result<void> write_archive(const std::filesystem::path& output,
                           const std::vector<Entry>& entries) {
  if (output.empty() || output.filename().empty())
    return Error("backup: refusing an empty output path");
  const std::filesystem::path parent = output.parent_path();
  if (!parent.empty()) {
    const Result<void> dir = ensure_dir(parent);
    if (dir.is_err())
      return std::move(dir).error();
  }

  for (int attempt = 0; attempt < kMaxTempAttempts; ++attempt) {
    const std::string tmp = output.string() + ".tmp." +
                            std::to_string(::getpid()) + "." +
                            std::to_string(attempt);
    const int raw =
        ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (raw < 0) {
      if (errno == EEXIST)
        continue;
      return Error("backup: open " + tmp + ": " + errno_text(errno));
    }
    FdGuard fd(raw);
    const Result<void> body = write_body(fd.get(), entries);
    if (body.is_err()) {
      fd.close_now();
      ::unlink(tmp.c_str());
      return std::move(body).error();
    }
    if (::fsync(fd.get()) != 0) {
      const std::string why = errno_text(errno);
      fd.close_now();
      ::unlink(tmp.c_str());
      return Error("backup: fsync " + tmp + ": " + why);
    }
    fd.close_now();
    std::error_code rename_ec;
    std::filesystem::rename(tmp, output, rename_ec);
    if (rename_ec) {
      ::unlink(tmp.c_str());
      return Error("backup: rename " + tmp + " -> " + output.string() + ": " +
                   rename_ec.message());
    }
    return {};
  }
  return Error("backup: temp-name collision at " + output.string());
}

Result<bool> service_active(const std::string& service,
                            std::atomic<bool>* cancel) {
  const Result<int> code =
      systemctl_user_code({"is-active", service + ".service"},
                          std::chrono::milliseconds(20000), cancel);
  if (code.is_err())
    return std::move(code).error();
  return code.value() == 0;
}

}  // namespace

std::filesystem::path default_backup_path(const Paths& paths,
                                          const std::string_view service) {
  return paths.state_dir / "backups" / (std::string(service) + ".tar");
}

Result<std::string> tar_create(const std::vector<TarFile>& files) {
  std::string out;
  for (const TarFile& file : files) {
    const std::uint64_t size =
        file.typeflag == '0' ? file.data.size() : std::uint64_t{0};
    Result<std::string> header =
        build_header(file.name, file.mode, file.typeflag, size,
                     file.typeflag == '2' ? file.link_target : std::string());
    if (header.is_err())
      return std::move(header).error();
    out += header.value();
    if (file.typeflag == '0') {
      out += file.data;
      const std::size_t pad =
          static_cast<std::size_t>((kBlock - (size % kBlock)) % kBlock);
      out.append(pad, '\0');
    }
  }
  out.append(2 * kBlock, '\0');
  return out;
}

Result<BackupReport> backup_to_archive(const Paths& paths,
                                       const std::string_view service,
                                       const std::filesystem::path& output,
                                       std::atomic<bool>* cancel) {
  static_cast<void>(cancel);
  if (!validate_unit_name(service))
    return Error("backup: invalid service name: '" + std::string(service) +
                 "'");

  const std::filesystem::path data_dir = paths.services_dir / service;
  const std::filesystem::path secrets =
      paths.secrets_dir / (std::string(service) + ".env");

  std::error_code ec;
  const bool has_data = std::filesystem::is_directory(data_dir, ec);
  if (ec)
    return Error("backup: stat " + data_dir.string() + ": " + ec.message());
  const bool has_secrets = std::filesystem::is_regular_file(secrets, ec);
  if (ec)
    return Error("backup: stat " + secrets.string() + ": " + ec.message());
  if (!has_data && !has_secrets) {
    return Error("backup: nothing to archive for '" + std::string(service) +
                 "' — no data dir (" + data_dir.string() +
                 ") and no secrets file (" + secrets.string() + ")");
  }

  std::vector<Entry> entries;
  BackupReport report;
  if (has_data) {
    const Result<void> walked = collect_dir(data_dir, &entries, &report);
    if (walked.is_err())
      return std::move(walked).error();
  }
  if (has_secrets) {
    const std::uint64_t size = std::filesystem::file_size(secrets, ec);
    if (ec)
      return Error("backup: size " + secrets.string() + ": " + ec.message());
    Entry entry = make_entry("secrets.env", '0', 0600);
    entry.source = secrets;
    entry.size = size;
    entries.push_back(std::move(entry));
    report.had_secrets = true;
    ++report.files;
    report.bytes += size;
  }

  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.name < b.name; });

  const Result<void> wrote = write_archive(output, entries);
  if (wrote.is_err())
    return std::move(wrote).error();
  return report;
}

Result<BackupReport> backup_service(const Paths& paths,
                                    const std::string_view service,
                                    const std::filesystem::path& output,
                                    std::atomic<bool>* cancel) {
  if (!validate_unit_name(service))
    return Error("backup: invalid service name: '" + std::string(service) +
                 "'");
  const Result<bool> active = service_active(std::string(service), cancel);
  if (active.is_err())
    return std::move(active).error();
  if (active.value()) {
    return Error("backup: '" + std::string(service) +
                 "' is running; stop it first so the archive is consistent");
  }
  return backup_to_archive(paths, service, output, cancel);
}

Result<RestoreReport> restore_from_archive(const Paths& paths,
                                           const std::string_view service,
                                           const std::string_view archive,
                                           const bool force) {
  if (!validate_unit_name(service))
    return Error("restore: invalid service name: '" + std::string(service) +
                 "'");

  const Result<std::vector<TarEntry>> listed = tar_list(archive);
  if (listed.is_err())
    return std::move(listed).error();

  // The archive is OURS: only `data/` and `secrets.env` may appear at the
  // top level. Anything else is refused, so a tampered archive cannot drop a
  // file somewhere the restore path never intended (§11).
  for (const TarEntry& entry : listed.value()) {
    std::string name = entry.name;
    if (!name.empty() && name.back() == '/')
      name.pop_back();
    if (name.empty())
      return Error("restore: archive has an empty entry name");
    if (name == "secrets.env")
      continue;
    const std::size_t slash = name.find('/');
    const std::string first =
        slash == std::string::npos ? name : name.substr(0, slash);
    if (first != "data") {
      return Error("restore: unexpected archive entry '" + entry.name + "'");
    }
  }

  const Result<void> services = ensure_dir(paths.services_dir);
  if (services.is_err())
    return std::move(services).error();
  const std::filesystem::path staging =
      paths.services_dir / (std::string(service) + ".restore-staging");
  const Result<std::size_t> cleared = remove_tree(staging);
  if (cleared.is_err())
    return std::move(cleared).error();
  const Result<void> staged = ensure_dir(staging);
  if (staged.is_err())
    return std::move(staged).error();

  struct StagingGuard {
    std::filesystem::path dir;
    ~StagingGuard() {
      const Result<std::size_t> removed = remove_tree(dir);
      static_cast<void>(removed);
    }
  } guard{staging};

  const Result<std::size_t> extracted = tar_extract(archive, staging);
  if (extracted.is_err())
    return std::move(extracted).error();

  RestoreReport report;
  for (const TarEntry& entry : listed.value()) {
    if (entry.typeflag == '0')
      ++report.files;
  }

  std::error_code ec;
  const std::filesystem::path source_data = staging / "data";
  const std::filesystem::path target = paths.services_dir / service;
  if (std::filesystem::is_directory(source_data, ec)) {
    if (ec)
      return Error("restore: stat " + source_data.string() + ": " +
                   ec.message());
    if (std::filesystem::exists(target, ec)) {
      if (ec)
        return Error("restore: stat " + target.string() + ": " + ec.message());
      if (!force) {
        return Error("restore: '" + std::string(service) +
                     "' already has data at " + target.string() +
                     "; pass --force to replace it");
      }
      const Result<std::size_t> removed = remove_tree(target);
      if (removed.is_err())
        return std::move(removed).error();
    }
    std::filesystem::rename(source_data, target, ec);
    if (ec) {
      return Error("restore: placing data at " + target.string() + ": " +
                   ec.message());
    }
    report.restored_data = true;
  }

  const std::filesystem::path source_secrets = staging / "secrets.env";
  if (std::filesystem::is_regular_file(source_secrets, ec)) {
    if (ec)
      return Error("restore: stat " + source_secrets.string() + ": " +
                   ec.message());
    const Result<std::string> bytes = read_file(source_secrets);
    if (bytes.is_err())
      return std::move(bytes).error();
    const Result<void> dir = ensure_dir(paths.secrets_dir);
    if (dir.is_err())
      return std::move(dir).error();
    const Result<void> written =
        atomic_write(paths.secrets_dir / (std::string(service) + ".env"),
                     bytes.value(), kPermPrivate);
    if (written.is_err())
      return std::move(written).error();
    report.restored_secrets = true;
  }

  return report;
}

Result<RestoreReport> restore_service(const Paths& paths,
                                      const std::string_view service,
                                      const std::filesystem::path& archive,
                                      const bool force,
                                      std::atomic<bool>* cancel) {
  if (!validate_unit_name(service))
    return Error("restore: invalid service name: '" + std::string(service) +
                 "'");
  const Result<bool> active = service_active(std::string(service), cancel);
  if (active.is_err())
    return std::move(active).error();
  if (active.value()) {
    return Error("restore: '" + std::string(service) +
                 "' is running; stop it first (its open files would race the "
                 "restore)");
  }
  const Result<std::string> bytes = read_file(archive);
  if (bytes.is_err())
    return std::move(bytes).error();
  return restore_from_archive(paths, service, bytes.value(), force);
}

}  // namespace berth
