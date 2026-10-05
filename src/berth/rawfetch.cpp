// src/berth/rawfetch.cpp — see rawfetch.h for the contract. The tar
// reader below is deliberately small: ustar headers plus the pax
// extensions git archive actually emits (path, linkpath, size overrides
// and the global header). Anything else is rejected loudly, not guessed.

#include "rawfetch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../atomic.h"
#include "../berth/httpclient.h"
#include "validate.h"
#include "../util/errno.h"
#include "../util/gzip.h"
#include "../util/fd.h"
#include "../util/strutil.h"

namespace berth {

namespace {
constexpr std::size_t kBlockSize = 512;
constexpr std::string_view kGitHubHost = "github.com";
constexpr std::string_view kCodeloadHost = "codeload.github.com";
}  // namespace

bool archive_path_safe(const std::string_view path) {
  if (path.empty()) return false;
  if (path.front() == '/') return false;          // absolute
  if (path.find('\0') != std::string_view::npos) return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view component =
        path.substr(start, slash == std::string_view::npos
                               ? std::string_view::npos
                               : slash - start);
    if (component == "..") return false;  // traversal
    if (slash == std::string_view::npos) break;
    start = slash + 1;
    if (start == path.size()) break;  // trailing slash: fine (dirs)
  }
  return true;
}

bool link_target_escapes(const std::string_view from_dir,
                         const std::string_view target) {
  if (target.empty()) return true;
  if (target.front() == '/') return true;  // absolute target always escapes
  // Resolve components against from_dir without touching the filesystem.
  std::size_t depth = 0;
  if (!from_dir.empty() && from_dir != ".") {
    std::size_t start = 0;
    while (start <= from_dir.size()) {
      const std::size_t slash = from_dir.find('/', start);
      const std::string_view component =
          from_dir.substr(start, slash == std::string_view::npos
                                     ? std::string_view::npos
                                     : slash - start);
      if (component == "..") {
        if (depth == 0) return true;
        --depth;
      } else if (!component.empty() && component != ".") {
        ++depth;
      }
      if (slash == std::string_view::npos) break;
      start = slash + 1;
    }
  }
  std::size_t start = 0;
  while (start <= target.size()) {
    const std::size_t slash = target.find('/', start);
    const std::string_view component =
        target.substr(start, slash == std::string_view::npos
                                 ? std::string_view::npos
                                 : slash - start);
    if (component == "..") {
      if (depth == 0) return true;
      --depth;
    } else if (!component.empty() && component != ".") {
      ++depth;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return false;
}

namespace {

struct RawEntry {
  std::string name;
  std::uint64_t size = 0;
  char typeflag = '0';
  std::string link_target;
  std::uint32_t mode = 0;
  std::size_t data_offset = 0;  // into the archive bytes
};

[[nodiscard]] bool all_zero(const std::string_view block) {
  return std::all_of(block.begin(), block.end(),
                     [](const char c) { return c == '\0'; });
}

// Octal field: spaces and NULs pad; digits parse. Empty -> 0.
[[nodiscard]] Result<std::uint64_t> parse_octal(const std::string_view field,
                                                const std::string& what) {
  std::size_t start = 0;
  while (start < field.size() &&
         (field[start] == ' ' || field[start] == '\0')) {
    ++start;
  }
  std::size_t end = start;
  while (end < field.size() && field[end] >= '0' && field[end] <= '7') {
    ++end;
  }
  if (end == start) {
    if (start == field.size()) return std::uint64_t{0};  // all padding
    return Error("tar: non-octal " + what);
  }
  std::uint64_t value = 0;
  for (std::size_t i = start; i < end; ++i) {
    value = value * 8 + static_cast<std::uint64_t>(field[i] - '0');
  }
  return value;
}

[[nodiscard]] std::string_view trimmed(const std::string_view field) {
  const std::size_t nul = field.find('\0');
  std::string_view out = nul == std::string_view::npos
                             ? field
                             : field.substr(0, nul);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) {
    out.remove_suffix(1);
  }
  return out;
}

[[nodiscard]] Result<std::uint64_t> header_checksum(
    const std::string_view header) {
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
  if (stored.is_err()) return std::move(stored).error();
  const std::uint64_t value = stored.value();
  if (value != unsigned_sum &&
      static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_sum)) != value) {
    return Error("tar: header checksum mismatch");
  }
  return value;
}

// Applies "len key=value\n" pax records; path/linkpath/size matter.
[[nodiscard]] Result<void> apply_pax_records(const std::string_view records,
                                             std::string& path,
                                             std::string& linkpath,
                                             std::uint64_t& size,
                                             bool& size_overridden) {
  std::size_t pos = 0;
  while (pos < records.size()) {
    const std::size_t space = records.find(' ', pos);
    if (space == std::string_view::npos) {
      return Error("tar: malformed pax record");
    }
    std::uint64_t length = 0;
    for (std::size_t i = pos; i < space; ++i) {
      if (records[i] < '0' || records[i] > '9') {
        return Error("tar: malformed pax length");
      }
      length = length * 10 + static_cast<std::uint64_t>(records[i] - '0');
    }
    if (length == 0 || pos + length > records.size()) {
      return Error("tar: malformed pax length");
    }
    const std::string_view record = records.substr(pos, length);
    const std::size_t eq = record.find('=');
    if (eq == std::string_view::npos) {
      return Error("tar: malformed pax record");
    }
    std::string_view key = record.substr(space - pos + 1, eq - (space - pos) - 1);
    std::string_view value = record.substr(eq + 1);
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
    if (key == "path") {
      path = std::string(value);
    } else if (key == "linkpath") {
      linkpath = std::string(value);
    } else if (key == "size") {
      Result<std::uint64_t> parsed = parse_octal(value, "pax size");
      if (parsed.is_err()) return std::move(parsed).error();
      size = parsed.value();
      size_overridden = true;
    }
    pos += length;
  }
  return {};
}

[[nodiscard]] Result<std::vector<RawEntry>> tar_parse(const std::string_view bytes) {
  std::vector<RawEntry> entries;
  std::size_t pos = 0;
  // Pending pax per-file overrides; the global header only records them.
  std::string pending_path;
  std::string pending_linkpath;
  std::uint64_t pending_size = 0;
  bool pending_size_set = false;

  while (pos < bytes.size()) {
    const std::string_view block = bytes.substr(pos, kBlockSize);
    if (block.size() < kBlockSize) {
      return Error("tar: truncated header block");
    }
    if (all_zero(block)) {
      // Two zero blocks terminate; everything after is padding.
      const std::size_t next = pos + kBlockSize;
      if (next + kBlockSize <= bytes.size() &&
          all_zero(bytes.substr(next, kBlockSize))) {
        return entries;
      }
      return Error("tar: missing end-of-archive terminator");
    }

    Result<std::uint64_t> checksum = header_checksum(block);
    if (checksum.is_err()) return std::move(checksum).error();

    std::string_view name = trimmed(block.substr(0, 100));
    const std::string_view prefix = trimmed(block.substr(345, 155));
    if (block[124] == '\x80') {
      return Error("tar: base-256 sizes are not supported");
    }
    Result<std::uint64_t> size = parse_octal(block.substr(124, 12), "size");
    if (size.is_err()) return std::move(size).error();
    const char typeflag = block[156] == '\0' ? '0' : block[156];
    std::string link = std::string(trimmed(block.substr(157, 100)));
    const Result<std::uint64_t> mode_raw =
        parse_octal(block.substr(100, 8), "mode");
    if (mode_raw.is_err()) return std::move(mode_raw).error();

    if (!prefix.empty()) {
      name = std::string(prefix) + "/" + std::string(name);
    }

    const std::size_t data_offset = pos + kBlockSize;
    const std::size_t data_len = static_cast<std::size_t>(size.value());
    const std::size_t padded = ((data_len + kBlockSize - 1) / kBlockSize) * kBlockSize;
    if (data_offset + padded > bytes.size()) {
      return Error("tar: truncated entry data for '" + std::string(name) + "'");
    }

    if (typeflag == 'x' || typeflag == 'g') {
      const std::string_view records = bytes.substr(data_offset, data_len);
      std::string path = std::string(name);
      std::string linkpath = link;
      std::uint64_t pax_size = size.value();
      bool size_overridden = false;
      Result<void> applied = apply_pax_records(records, path, linkpath,
                                               pax_size, size_overridden);
      if (applied.is_err()) return std::move(applied).error();
      if (typeflag == 'x') {
        pending_path = std::move(path);
        pending_linkpath = std::move(linkpath);
        pending_size = pax_size;
        pending_size_set = size_overridden;
      }
      pos = data_offset + padded;
      continue;
    }
    if (typeflag == 'L') {
      return Error("tar: GNU long-name extension is not supported");
    }

    RawEntry entry;
    entry.name = pending_path.empty() ? std::string(name) : pending_path;
    entry.size = pending_size_set ? pending_size : size.value();
    entry.typeflag = typeflag;
    entry.link_target =
        pending_linkpath.empty() ? std::move(link) : pending_linkpath;
    entry.mode = static_cast<std::uint32_t>(mode_raw.value());
    entry.data_offset = data_offset;
    entries.push_back(std::move(entry));

    pending_path.clear();
    pending_linkpath.clear();
    pending_size = 0;
    pending_size_set = false;

    pos = data_offset + padded;
  }
  return Error("tar: archive ended without an end-of-archive terminator");
}

[[nodiscard]] Result<std::filesystem::path> extract_entry(
    const RawEntry& entry, const std::string_view bytes,
    const std::filesystem::path& dest) {
  if (!archive_path_safe(entry.name)) {
    return Error("tar_extract: unsafe entry path '" + entry.name + "'");
  }
  const std::filesystem::path target = dest / std::filesystem::path(entry.name);

  switch (entry.typeflag) {
    case '5': {  // dir
      std::error_code ec;
      std::filesystem::create_directories(target, ec);
      if (ec) {
        return Error("tar_extract: mkdir " + target.string() + ": " + ec.message());
      }
      return target;
    }
    case '0': {  // regular file
      std::error_code ec;
      std::filesystem::create_directories(target.parent_path(), ec);
      if (ec) {
        return Error("tar_extract: mkdir " + target.parent_path().string() +
                     ": " + ec.message());
      }
      const bool executable = (entry.mode & 0100) != 0;
      const FdGuard fd(::open(target.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                              executable ? 0755 : 0644));
      if (fd.get() < 0) {
        return Error("tar_extract: open " + target.string() +
                     (errno == EEXIST ? " (duplicate entry)" : ": ") +
                     errno_text(errno));
      }
      const char* cursor = bytes.data() + entry.data_offset;
      std::size_t left = static_cast<std::size_t>(entry.size);
      while (left > 0) {
        const ssize_t n = ::write(fd.get(), cursor, left);
        if (n < 0) {
          if (errno == EINTR) continue;
          return Error("tar_extract: write " + target.string() + ": " +
                       errno_text(errno));
        }
        cursor += n;
        left -= static_cast<std::size_t>(n);
      }
      return target;
    }
    case '2': {  // symlink
      const std::string dir_of = std::filesystem::path(entry.name)
                                     .parent_path()
                                     .string();
      if (link_target_escapes(dir_of, entry.link_target)) {
        return Error("tar_extract: symlink '" + entry.name + "' -> '" +
                     entry.link_target + "' escapes the destination");
      }
      std::error_code ec;
      std::filesystem::create_directories(target.parent_path(), ec);
      if (ec) {
        return Error("tar_extract: mkdir " + target.parent_path().string() +
                     ": " + ec.message());
      }
      if (::symlink(entry.link_target.c_str(), target.c_str()) != 0) {
        return Error("tar_extract: symlink " + target.string() +
                     (errno == EEXIST ? " (duplicate entry)" : ": ") +
                     errno_text(errno));
      }
      return target;
    }
    case '1': {  // hardlink
      if (!archive_path_safe(entry.link_target)) {
        return Error("tar_extract: hardlink '" + entry.name + "' has unsafe target '" +
                     entry.link_target + "'");
      }
      if (link_target_escapes(std::filesystem::path(entry.name).parent_path().string(),
                              entry.link_target)) {
        return Error("tar_extract: hardlink '" + entry.name + "' escapes the destination");
      }
      const std::filesystem::path from = dest / std::filesystem::path(entry.link_target);
      std::error_code ec;
      std::filesystem::create_directories(target.parent_path(), ec);
      if (ec) {
        return Error("tar_extract: mkdir " + target.parent_path().string() +
                     ": " + ec.message());
      }
      if (::link(from.c_str(), target.c_str()) != 0) {
        return Error("tar_extract: link " + target.string() + ": " +
                     errno_text(errno));
      }
      return target;
    }
    default:
      return Error("tar_extract: refusing entry '" + entry.name +
                   "' with unsupported typeflag '" +
                   std::string(1, entry.typeflag) + "'");
  }
}

}  // namespace

Result<std::vector<TarEntry>> tar_list(const std::string_view bytes) {
  Result<std::vector<RawEntry>> parsed = tar_parse(bytes);
  if (parsed.is_err()) return std::move(parsed).error();
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
  if (parsed.is_err()) return std::move(parsed).error();
  Result<void> dir = ensure_dir(dest);
  if (dir.is_err()) return std::move(dir).error();
  std::size_t extracted = 0;
  for (const RawEntry& entry : parsed.value()) {
    Result<std::filesystem::path> made = extract_entry(entry, bytes, dest);
    if (made.is_err()) return std::move(made).error();
    ++extracted;
  }
  return extracted;
}

Result<std::size_t> tar_extract_gz(const std::string_view gzipped,
                                   const std::filesystem::path& dest,
                                   const std::size_t max_out) {
  Result<std::string> plain = gunzip(gzipped, max_out);
  if (plain.is_err()) return std::move(plain).error();
  return tar_extract(plain.value(), dest);
}

Result<std::vector<TreeFile>> parse_tree_response(const std::string_view body) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& e) {
    return Error("trees api: parse: " + std::string(e.what()));
  }
  if (!doc.is_object() || !doc.contains("tree") || !doc["tree"].is_array()) {
    return Error("trees api: no tree array");
  }
  if (doc.value("truncated", false) == true) {
    return Error("trees api: listing truncated by GitHub; refusing incomplete data");
  }
  std::vector<TreeFile> out;
  for (const nlohmann::json& item : doc["tree"]) {
    if (!item.is_object() || !item.contains("path") || !item.contains("type")) {
      continue;
    }
    if (item["type"] != "blob") continue;  // trees/commits are not files
    TreeFile file;
    file.path = item["path"].get<std::string>();
    file.sha = item.contains("sha") && item["sha"].is_string()
                   ? item["sha"].get<std::string>()
                   : std::string();
    file.size = item.contains("size") && item["size"].is_number_unsigned()
                    ? item["size"].get<std::uint64_t>()
                    : 0;
    out.push_back(std::move(file));
  }
  return out;
}

Result<std::vector<TreeFile>> fetch_tree(const std::string& owner_repo,
                                         const std::string& sha,
                                         const Paths& paths,
                                         const std::string* token,
                                         std::atomic<bool>* cancel) {
  Result<void> valid = validate_owner_repo(owner_repo);
  if (valid.is_err()) return std::move(valid).error();
  valid = validate_commit_sha(sha);
  if (valid.is_err()) return std::move(valid).error();

  HttpGetOptions options;
  options.kind = RequestKind::kApi;
  options.purpose = "github-trees";
  options.bearer_token = token;
  options.cancel = cancel;
  const std::string url = std::string("https://api.github.com/repos/") +
                          owner_repo + "/git/trees/" + sha + "?recursive=1";
  Result<HttpReply> reply = http_get(url, options, paths);
  if (reply.is_err()) return std::move(reply).error();
  if (reply.value().status != 200) {
    return Error("rawfetch: trees api answered status " +
                 std::to_string(reply.value().status) + " for " + owner_repo +
                 "@" + sha);
  }
  return parse_tree_response(reply.value().body);
}

Result<std::filesystem::path> fetch_and_extract(
    const std::string& owner_repo, const std::string& sha,
    const std::filesystem::path& dest_dir, const Paths& paths,
    const std::string* token, std::atomic<bool>* cancel) {
  Result<void> valid = validate_owner_repo(owner_repo);
  if (valid.is_err()) return std::move(valid).error();
  valid = validate_commit_sha(sha);
  if (valid.is_err()) return std::move(valid).error();

  HttpGetOptions options;
  options.kind = RequestKind::kApi;
  options.purpose = "github-archive";
  options.bearer_token = token;
  options.cancel = cancel;
  const std::string url = std::string("https://") +
                          std::string(kCodeloadHost) + "/" + owner_repo +
                          "/tar.gz/" + sha;
  Result<HttpReply> reply = http_get(url, options, paths);
  if (reply.is_err()) return std::move(reply).error();
  if (reply.value().status != 200) {
    return Error("rawfetch: codeload answered status " +
                 std::to_string(reply.value().status) + " for " + owner_repo +
                 "@" + sha);
  }

  Result<std::size_t> extracted =
      tar_extract_gz(reply.value().body, dest_dir);
  if (extracted.is_err()) return std::move(extracted).error();

  // The tarball carries exactly one top-level directory (owner-repo-sha).
  std::vector<std::filesystem::path> top_dirs;
  for (const auto& item : std::filesystem::directory_iterator(dest_dir)) {
    if (item.is_directory()) top_dirs.push_back(item.path());
  }
  if (top_dirs.size() != 1) {
    return Error("rawfetch: expected a single top-level directory in the " +
                 std::string(kGitHubHost) + " tarball, found " +
                 std::to_string(top_dirs.size()));
  }
  return top_dirs.front();
}

}  // namespace berth
