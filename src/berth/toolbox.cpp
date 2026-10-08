#include "berth/toolbox.h"

#include <string>

#include "../core/atomic.h"
#include "../util/fs.h"
#include "../util/gzip.h"
#include "../util/sha256.h"
#include "../util/strutil.h"
#include "core/netlog.h"
#include "net/fetch.h"
#include "net/github_api.h"
#include "net/tar.h"
#include "util/fs.h"

namespace berth {

namespace {

constexpr std::size_t toolbox_max_tool_bytes = 512ull * 1024 * 1024;

Result<std::string> sha256_of_file(const std::filesystem::path& path) {
  Result<std::string> bytes = read_file(path);
  if (bytes.is_err())
    return std::move(bytes).error();
  return sha256_hex(bytes.value());
}

}  // namespace

Result<void> verify_sha256_file(const std::filesystem::path& path,
                                const std::string& expected_sha256) {
  Result<std::string> actual = sha256_of_file(path);
  if (actual.is_err())
    return std::move(actual).error();
  std::string want = expected_sha256;
  want = trim(want);
  for (char& c : want) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  if (want.size() != 64) {
    return Error("toolbox: expected sha256 is not 64 hex chars");
  }
  if (actual.value() != want) {
    return Error("toolbox: sha256 mismatch for " + path.filename().string() +
                 ": expected " + want + ", got " + actual.value());
  }
  return {};
}

Result<std::filesystem::path> download_verified(
    const std::string& url, const std::string& expected_sha256,
    const std::filesystem::path& staging_dir, const Paths& paths,
    std::atomic<bool>* cancel) {
  Result<void> dir = ensure_dir(staging_dir);
  if (dir.is_err())
    return std::move(dir).error();
  const std::filesystem::path staged = staging_dir / "download.tmp";

  HttpGetOptions options;
  options.kind = RequestKind::kApi;
  options.purpose = "toolbox";
  options.follow_allowlisted_redirects = true;
  options.cancel = cancel;
  Result<HttpReply> reply = http_get(url, options, paths);
  if (reply.is_err())
    return std::move(reply).error();
  if (reply.value().status != 200) {
    return Error("toolbox: download answered status " +
                 std::to_string(reply.value().status) + " for " + url);
  }

  Result<void> written = atomic_write(staged, reply.value().body, kPermPrivate);
  if (written.is_err())
    return std::move(written).error();

  const Result<void> verified = verify_sha256_file(staged, expected_sha256);
  if (verified.is_err()) {
    Result<std::size_t> removed = remove_tree(staged);
    static_cast<void>(removed);
    return std::move(verified).error();
  }
  return staged;
}

Result<std::filesystem::path> install_pinned_tool(const PinnedTool& tool,
                                                  const Paths& paths,
                                                  std::atomic<bool>* cancel) {
  if (tool.sha256.empty()) {
    return Error("toolbox: " + tool.name + " has no sha256 pin");
  }
  // `inner_path` is the recipe's `member` and is interpolated into the install
  // path: a user recipe with member "../../.config/systemd/user/x.service"
  // would otherwise write outside install_dir (§11: validate every value that
  // reaches a path). tar_list does not apply archive_path_safe either.
  if (!archive_path_safe(tool.inner_path)) {
    return Error("toolbox: " + tool.name + " member is not a safe relative "
                 "path: '" + tool.inner_path + "'");
  }
  const std::filesystem::path staging =
      paths.state_dir / "toolbox" / (tool.name + ".staging");
  // A failed install (bad archive, missing member, write error) must not leave
  // the verified download behind.
  struct StagingGuard {
    std::filesystem::path dir;
    ~StagingGuard() {
      Result<std::size_t> removed = remove_tree(dir);
      static_cast<void>(removed);
    }
  } guard{staging};

  const Result<std::filesystem::path> downloaded =
      download_verified(tool.url, tool.sha256, staging, paths, cancel);
  if (downloaded.is_err())
    return std::move(downloaded).error();

  const Result<void> install_dir = ensure_dir(tool.install_dir);
  if (install_dir.is_err())
    return std::move(install_dir).error();
  const std::filesystem::path target = tool.install_dir / tool.inner_path;

  std::string payload;
  if (tool.archive == ToolArchive::kTarGz) {
    Result<std::string> compressed = read_file(downloaded.value());
    if (compressed.is_err())
      return std::move(compressed).error();
    Result<std::string> plain =
        gunzip(compressed.value(), toolbox_max_tool_bytes);
    if (plain.is_err())
      return std::move(plain).error();
    std::string archive_bytes = std::move(plain).value();
    Result<std::vector<TarEntry>> entries = tar_list(archive_bytes);
    if (entries.is_err())
      return std::move(entries).error();
    std::string_view want = tool.inner_path;
    const auto found = std::find_if(
        entries.value().begin(), entries.value().end(),
        [&](const TarEntry& entry) {
          return entry.name == want || entry.name == "./" + std::string(want);
        });
    if (found == entries.value().end()) {
      return Error("toolbox: " + tool.name + " archive has no member '" +
                   tool.inner_path + "'");
    }
    if (found->size > toolbox_max_tool_bytes) {
      return Error("toolbox: " + tool.name + " member exceeds the size cap");
    }
    // A pax `size` override is not re-validated by tar_parse against the
    // archive length, so the member's declared extent must be checked before
    // substr, or a crafted archive silently installs a truncated payload.
    if (found->data_offset > archive_bytes.size() ||
        found->size > archive_bytes.size() - found->data_offset) {
      return Error("toolbox: " + tool.name + " member '" + tool.inner_path +
                   "' extends past the archive");
    }
    payload = std::string_view(archive_bytes)
                  .substr(found->data_offset,
                          static_cast<std::size_t>(found->size));
  } else {
    Result<std::string> raw = read_file(downloaded.value());
    if (raw.is_err())
      return std::move(raw).error();
    payload = std::move(raw).value();
  }

  std::filesystem::perms perms =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  if (tool.executable) {
    perms = perms | std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec;
  }

  Result<void> parent_dir = ensure_dir(target.parent_path());
  if (parent_dir.is_err())
    return std::move(parent_dir).error();
  Result<void> placed = atomic_write(target, payload, perms);
  if (placed.is_err())
    return std::move(placed).error();

  return target;
}

}  // namespace berth
