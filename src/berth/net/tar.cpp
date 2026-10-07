// src/berth/net/tar.cpp — see tar.h for the contract.

#include "tar.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include "../../util/errno.h"
#include "../../util/fd.h"
#include "../../util/fs.h"
#include "../../util/gzip.h"

namespace berth {

namespace {

constexpr std::size_t kBlockSize = 512;

struct RawEntry {
  std::string name;
  std::uint64_t size = 0;
  char typeflag = '0';
  std::string link_target;
  std::uint32_t mode = 0;
  std::size_t data_offset = 0;  // into the archive bytes
};

// Per-file overrides a pax header may carry into the NEXT entry.
struct PaxOverrides {
  std::string path;
  std::string linkpath;
  std::uint64_t size = 0;
  bool size_set = false;

  void clear() { *this = PaxOverrides{}; }
};

// The fixed part of a ustar header, already validated and split out.
struct Header {
  std::string name;
  std::uint64_t size = 0;
  std::uint64_t mode = 0;
  char typeflag = '0';
  std::string link;
  std::size_t data_offset = 0;
  std::size_t padded_size = 0;
};

bool all_zero(const std::string_view block) {
  return std::all_of(block.begin(), block.end(),
                     [](const char c) { return c == '\0'; });
}

// Octal field: spaces and NULs pad; digits parse. All padding -> 0.
Result<std::uint64_t> parse_octal(const std::string_view field,
                                  const std::string& what) {
  std::size_t start = 0;
  while (start < field.size() &&
         (field[start] == ' ' || field[start] == '\0')) {
    ++start;
  }
  std::size_t end = start;
  while (end < field.size() && field[end] >= '0' && field[end] <= '7') ++end;
  if (end == start) {
    if (start == field.size())
      return std::uint64_t{0};
    return Error("tar: non-octal " + what);
  }
  std::uint64_t value = 0;
  for (std::size_t i = start; i < end; ++i) {
    value = value * 8 + static_cast<std::uint64_t>(field[i] - '0');
  }
  return value;
}

// NUL-terminated, space-padded field. Unlike strutil::trim this must stop at
// the first NUL: tar fields are fixed-width buffers, not text.
std::string_view trimmed(const std::string_view field) {
  const std::size_t nul = field.find('\0');
  std::string_view out =
      nul == std::string_view::npos ? field : field.substr(0, nul);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) {
    out.remove_suffix(1);
  }
  return out;
}

Result<std::uint64_t> header_checksum(const std::string_view header) {
  std::uint32_t unsigned_sum = 0;
  std::int32_t signed_sum = 0;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    const bool in_checksum_field = i >= 148 && i < 156;
    const char c = in_checksum_field ? ' ' : header[i];
    unsigned_sum += static_cast<std::uint8_t>(c);
    signed_sum += static_cast<std::int8_t>(c);
  }
  const Result<std::uint64_t> stored =
      parse_octal(header.substr(148, 8), "checksum");
  if (stored.is_err())
    return std::move(stored).error();
  const std::uint64_t value = stored.value();
  if (value != unsigned_sum &&
      static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_sum)) !=
          value) {
    return Error("tar: header checksum mismatch");
  }
  return value;
}

Result<Header> parse_header_block(const std::string_view bytes,
                                  const std::size_t pos) {
  const std::string_view block = bytes.substr(pos, kBlockSize);
  const Result<std::uint64_t> checksum = header_checksum(block);
  if (checksum.is_err())
    return std::move(checksum).error();
  if (block[124] == '\x80') {
    return Error("tar: base-256 sizes are not supported");
  }

  const Result<std::uint64_t> size = parse_octal(block.substr(124, 12), "size");
  if (size.is_err())
    return std::move(size).error();
  const Result<std::uint64_t> mode = parse_octal(block.substr(100, 8), "mode");
  if (mode.is_err())
    return std::move(mode).error();

  Header header;
  header.name = std::string(trimmed(block.substr(0, 100)));
  const std::string_view prefix = trimmed(block.substr(345, 155));
  if (!prefix.empty())
    header.name = std::string(prefix) + "/" + header.name;
  header.size = size.value();
  header.mode = mode.value();
  header.typeflag = block[156] == '\0' ? '0' : block[156];
  header.link = std::string(trimmed(block.substr(157, 100)));
  header.data_offset = pos + kBlockSize;
  header.padded_size =
      ((header.size + kBlockSize - 1) / kBlockSize) * kBlockSize;
  if (header.data_offset + header.padded_size > bytes.size()) {
    return Error("tar: truncated entry data for '" + header.name + "'");
  }
  return header;
}

// Two zero blocks terminate the archive; anything after them is padding.
bool ends_archive(const std::string_view bytes, const std::size_t pos) {
  const std::size_t next = pos + kBlockSize;
  return next + kBlockSize <= bytes.size() &&
         all_zero(bytes.substr(next, kBlockSize));
}

Result<std::size_t> pax_record_length(const std::string_view records,
                                      const std::size_t pos) {
  const std::size_t space = records.find(' ', pos);
  if (space == std::string_view::npos)
    return Error("tar: malformed pax record");
  std::size_t length = 0;
  for (std::size_t i = pos; i < space; ++i) {
    if (records[i] < '0' || records[i] > '9') {
      return Error("tar: malformed pax length");
    }
    length = length * 10 + static_cast<std::size_t>(records[i] - '0');
  }
  if (length == 0 || pos + length > records.size()) {
    return Error("tar: malformed pax length");
  }
  return length;
}

// Applies "len key=value\n" pax records; path/linkpath/size matter and the
// rest are ignored. A record we cannot parse fails the whole archive: a
// silently dropped path override would extract to the wrong place.
Result<PaxOverrides> parse_pax_records(const std::string_view records) {
  PaxOverrides out;
  std::size_t pos = 0;
  while (pos < records.size()) {
    const Result<std::size_t> length = pax_record_length(records, pos);
    if (length.is_err())
      return std::move(length).error();
    const std::string_view record = records.substr(pos, length.value());

    const std::size_t eq = record.find('=');
    if (eq == std::string_view::npos) {
      return Error("tar: malformed pax record");
    }
    const std::size_t key_offset = records.find(' ', pos) - pos + 1;
    const std::string_view key = record.substr(key_offset, eq - key_offset);
    std::string_view value = record.substr(eq + 1);
    if (!value.empty() && value.back() == '\n')
      value.remove_suffix(1);

    if (key == "path") {
      out.path = std::string(value);
    } else if (key == "linkpath") {
      out.linkpath = std::string(value);
    } else if (key == "size") {
      const Result<std::uint64_t> size = parse_octal(value, "pax size");
      if (size.is_err())
        return std::move(size).error();
      out.size = size.value();
      out.size_set = true;
    }
    pos += length.value();
  }
  return out;
}

RawEntry to_entry(const Header& header, const PaxOverrides& pax) {
  RawEntry entry;
  entry.name = pax.path.empty() ? header.name : pax.path;
  entry.size = pax.size_set ? pax.size : header.size;
  entry.typeflag = header.typeflag;
  entry.link_target = pax.linkpath.empty() ? header.link : pax.linkpath;
  entry.mode = static_cast<std::uint32_t>(header.mode);
  entry.data_offset = header.data_offset;
  return entry;
}

Result<std::vector<RawEntry>> tar_parse(const std::string_view bytes) {
  std::vector<RawEntry> entries;
  PaxOverrides pending;
  std::size_t pos = 0;

  while (pos < bytes.size()) {
    if (pos + kBlockSize > bytes.size()) {
      return Error("tar: truncated header block");
    }
    if (all_zero(bytes.substr(pos, kBlockSize))) {
      if (ends_archive(bytes, pos))
        return entries;
      return Error("tar: missing end-of-archive terminator");
    }

    Result<Header> header = parse_header_block(bytes, pos);
    if (header.is_err())
      return std::move(header).error();

    const std::size_t next =
        header.value().data_offset + header.value().padded_size;
    if (header.value().typeflag == 'x' || header.value().typeflag == 'g') {
      const std::string_view records =
          bytes.substr(header.value().data_offset,
                       static_cast<std::size_t>(header.value().size));
      Result<PaxOverrides> applied = parse_pax_records(records);
      if (applied.is_err())
        return std::move(applied).error();
      // A global header ('g') is recorded but does not apply to an entry.
      if (header.value().typeflag == 'x')
        pending = std::move(applied).value();
      pos = next;
      continue;
    }
    if (header.value().typeflag == 'L') {
      return Error("tar: GNU long-name extension is not supported");
    }

    entries.push_back(to_entry(header.value(), pending));
    pending.clear();
    pos = next;
  }
  return Error("tar: archive ended without an end-of-archive terminator");
}

Result<void> make_parent_dir(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  if (ec) {
    return Error("tar_extract: mkdir " + file.parent_path().string() + ": " +
                 ec.message());
  }
  return {};
}

std::string duplicate_suffix(const int saved_errno) {
  return saved_errno == EEXIST ? " (duplicate entry)" : ": ";
}

Result<std::filesystem::path> extract_file(
    const RawEntry& entry, const std::string_view bytes,
    const std::filesystem::path& target) {
  const Result<void> parent = make_parent_dir(target);
  if (parent.is_err())
    return std::move(parent).error();

  const bool executable = (entry.mode & 0100) != 0;
  const FdGuard fd(::open(target.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          executable ? 0755 : 0644));
  if (fd.get() < 0) {
    return Error("tar_extract: open " + target.string() +
                 duplicate_suffix(errno) + errno_text(errno));
  }
  const std::string_view payload(bytes.data() + entry.data_offset,
                                 static_cast<std::size_t>(entry.size));
  const Result<void> written = write_all(fd.get(), payload);
  if (written.is_err()) {
    return Error("tar_extract: write " + target.string() + ": " +
                 written.error().ctx());
  }
  return target;
}

Result<std::filesystem::path> extract_directory(
    const std::filesystem::path& target) {
  std::error_code ec;
  std::filesystem::create_directories(target, ec);
  if (ec) {
    return Error("tar_extract: mkdir " + target.string() + ": " + ec.message());
  }
  return target;
}

Result<std::filesystem::path> extract_symlink(
    const RawEntry& entry, const std::filesystem::path& target) {
  const std::string dir_of =
      std::filesystem::path(entry.name).parent_path().string();
  if (link_target_escapes(dir_of, entry.link_target)) {
    return Error("tar_extract: symlink '" + entry.name + "' -> '" +
                 entry.link_target + "' escapes the destination");
  }
  const Result<void> parent = make_parent_dir(target);
  if (parent.is_err())
    return std::move(parent).error();
  if (::symlink(entry.link_target.c_str(), target.c_str()) != 0) {
    return Error("tar_extract: symlink " + target.string() +
                 duplicate_suffix(errno) + errno_text(errno));
  }
  return target;
}

Result<std::filesystem::path> extract_hardlink(
    const RawEntry& entry, const std::filesystem::path& target,
    const std::filesystem::path& dest) {
  if (!archive_path_safe(entry.link_target)) {
    return Error("tar_extract: hardlink '" + entry.name +
                 "' has unsafe target '" + entry.link_target + "'");
  }
  const std::string dir_of =
      std::filesystem::path(entry.name).parent_path().string();
  if (link_target_escapes(dir_of, entry.link_target)) {
    return Error("tar_extract: hardlink '" + entry.name +
                 "' escapes the destination");
  }
  const std::filesystem::path from =
      dest / std::filesystem::path(entry.link_target);
  const Result<void> parent = make_parent_dir(target);
  if (parent.is_err())
    return std::move(parent).error();
  if (::link(from.c_str(), target.c_str()) != 0) {
    return Error("tar_extract: link " + target.string() +
                 duplicate_suffix(errno) + errno_text(errno));
  }
  return target;
}

Result<std::filesystem::path> extract_entry(const RawEntry& entry,
                                            const std::string_view bytes,
                                            const std::filesystem::path& dest) {
  if (!archive_path_safe(entry.name)) {
    return Error("tar_extract: unsafe entry path '" + entry.name + "'");
  }
  const std::filesystem::path target = dest / std::filesystem::path(entry.name);

  switch (entry.typeflag) {
    case '5':
      return extract_directory(target);
    case '0':
      return extract_file(entry, bytes, target);
    case '2':
      return extract_symlink(entry, target);
    case '1':
      return extract_hardlink(entry, target, dest);
    default:
      return Error("tar_extract: refusing entry '" + entry.name +
                   "' with unsupported typeflag '" +
                   std::string(1, entry.typeflag) + "'");
  }
}

}  // namespace

bool archive_path_safe(const std::string_view path) {
  if (path.empty())
    return false;
  if (path.front() == '/')
    return false;  // absolute
  if (path.find('\0') != std::string_view::npos)
    return false;

  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view component = path.substr(
        start, slash == std::string_view::npos ? std::string_view::npos
                                               : slash - start);
    if (component == "..")
      return false;  // traversal
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
    // A trailing slash (directories) is fine.
  }
  return true;
}

namespace {

// Resolves path components into a depth counter without touching the
// filesystem: "a/../b" returns to the same depth, "b/.." to zero.
std::size_t depth_of(std::string_view path, std::size_t depth) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view component = path.substr(
        start, slash == std::string_view::npos ? std::string_view::npos
                                               : slash - start);
    if (component == "..") {
      if (depth == 0)
        return std::string_view::npos;  // above the root
      --depth;
    } else if (!component.empty() && component != ".") {
      ++depth;
    }
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return depth;
}

}  // namespace

bool link_target_escapes(const std::string_view from_dir,
                         const std::string_view target) {
  if (target.empty())
    return true;
  if (target.front() == '/')
    return true;  // absolute target always escapes

  const std::size_t base =
      (from_dir.empty() || from_dir == ".") ? 0 : depth_of(from_dir, 0);
  if (base == std::string_view::npos)
    return true;
  return depth_of(target, base) == std::string_view::npos;
}

Result<std::vector<TarEntry>> tar_list(const std::string_view bytes) {
  Result<std::vector<RawEntry>> parsed = tar_parse(bytes);
  if (parsed.is_err())
    return std::move(parsed).error();

  std::vector<TarEntry> out;
  out.reserve(parsed.value().size());
  for (const RawEntry& e : parsed.value()) {
    TarEntry entry;
    entry.name = std::move(e.name);
    entry.size = e.size;
    entry.typeflag = e.typeflag;
    entry.link_target = std::move(e.link_target);
    entry.mode = e.mode;
    entry.data_offset = e.data_offset;
    out.push_back(std::move(entry));
  }
  return out;
}

Result<std::size_t> tar_extract(const std::string_view bytes,
                                const std::filesystem::path& dest) {
  Result<std::vector<RawEntry>> parsed = tar_parse(bytes);
  if (parsed.is_err())
    return std::move(parsed).error();
  const Result<void> dir = ensure_dir(dest);
  if (dir.is_err())
    return std::move(dir).error();

  std::size_t extracted = 0;
  for (const RawEntry& entry : parsed.value()) {
    const Result<std::filesystem::path> made =
        extract_entry(entry, bytes, dest);
    if (made.is_err())
      return std::move(made).error();
    ++extracted;
  }
  return extracted;
}

Result<std::size_t> tar_extract_gz(const std::string_view gzipped,
                                   const std::filesystem::path& dest,
                                   const std::size_t max_out) {
  Result<std::string> plain = gunzip(gzipped, max_out);
  if (plain.is_err())
    return std::move(plain).error();
  return tar_extract(plain.value(), dest);
}

}  // namespace berth
