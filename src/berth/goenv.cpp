#include "goenv.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>

#include "../core/atomic.h"
#include "../util/errno.h"
#include "../util/fd.h"
#include "../util/fs.h"
#include "../util/gzip.h"
#include "net/fetch.h"
#include "net/github_api.h"
#include "net/tar.h"

namespace berth {

Result<std::string> escape_module_path(const std::string_view module) {
  if (module.empty())
    return Error("goenv: empty module path");
  std::string out;
  out.reserve(module.size() + 8);
  for (const char c : module) {
    if (c >= 'A' && c <= 'Z') {
      out += '!';
      out += static_cast<char>(c - 'A' + 'a');
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_' || c == '~' || c == '/' ||
                    c == '+';
    if (!ok) {
      return Error("goenv: bad character in module path: " +
                   std::string(module));
    }
    out += c;
  }
  return out;
}

Result<std::string> range_header_value(const std::size_t have,
                                       const std::size_t total) {
  if (total != 0 && have >= total) {
    return Error("goenv: nothing left to resume");
  }
  return "bytes=" + std::to_string(have) + "-";
}

Result<std::size_t> current_segment_size(const std::filesystem::path& partial) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(partial, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) {
    return Error("goenv: size " + partial.string() + ": " + ec.message());
  }
  if (ec)
    return std::size_t{0};
  return static_cast<std::size_t>(size);
}

Result<std::size_t> append_segment(const std::filesystem::path& partial,
                                   const std::string_view chunk) {
  const int fd =
      ::open(partial.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
             S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return Error("goenv: open " + partial.string() + ": " + errno_text(errno));
  }
  const std::size_t before = static_cast<std::size_t>(::lseek(fd, 0, SEEK_END));
  const Result<void> written = write_all(fd, chunk);
  ::close(fd);
  if (written.is_err()) {
    return Error("goenv: append " + partial.filename().string() + ": " +
                 written.error().ctx());
  }
  return before + chunk.size();
}

Result<void> finalize_segmented(const std::filesystem::path& partial,
                                const std::filesystem::path& dest,
                                const std::string& expected_sha256,
                                const std::size_t expected_size) {
  Result<std::size_t> have = current_segment_size(partial);
  if (have.is_err())
    return std::move(have).error();
  if (expected_size != 0 && have.value() != expected_size) {
    return Error("goenv: resumed download is " + std::to_string(have.value()) +
                 " bytes, expected " + std::to_string(expected_size));
  }
  Result<void> verified = verify_sha256_file(partial, expected_sha256);
  if (verified.is_err())
    return std::move(verified).error();
  Result<void> parent = ensure_dir(dest.parent_path());
  if (parent.is_err())
    return std::move(parent).error();
  std::error_code ec;
  std::filesystem::rename(partial, dest, ec);
  if (ec) {
    return Error("goenv: rename " + partial.string() + " -> " + dest.string() +
                 ": " + ec.message());
  }
  return {};
}

Result<std::filesystem::path> write_module_cache_entry(
    const std::filesystem::path& module_cache, const std::string& module,
    const std::string& version, const ModuleFiles& files) {
  Result<std::string> escaped = escape_module_path(module);
  if (escaped.is_err())
    return std::move(escaped).error();
  if (version.empty() || version.find('/') != std::string::npos) {
    return Error("goenv: bad module version: '" + version + "'");
  }
  const std::filesystem::path dir =
      module_cache / "cache" / "download" / escaped.value() / "@v";
  Result<void> made = ensure_dir(dir);
  if (made.is_err())
    return std::move(made).error();

  const std::pair<std::string, std::string> parts[] = {
      {version + ".info", files.info},
      {version + ".mod", files.mod},
      {version + ".zip", files.zip},
  };
  for (const auto& [name, body] : parts) {
    Result<void> written = atomic_write(dir / name, body, kPermPrivate);
    if (written.is_err())
      return std::move(written).error();
  }
  return dir;
}

Result<std::filesystem::path> install_go_toolchain(
    const PinnedTool& archive, const std::filesystem::path& tools_dir,
    const Paths& paths, std::atomic<bool>* cancel) {
  Result<std::filesystem::path> verified = download_verified(
      archive.url, archive.sha256,
      paths.state_dir / "toolbox" / (archive.name + ".staging"), paths, cancel);
  if (verified.is_err())
    return std::move(verified).error();
  Result<std::string> compressed = read_file(verified.value());
  if (compressed.is_err())
    return std::move(compressed).error();
  Result<std::string> bytes = gunzip(compressed.value(), 1024ull * 1024 * 1024);
  if (bytes.is_err())
    return std::move(bytes).error();
  Result<std::size_t> extracted = tar_extract(bytes.value(), tools_dir);
  if (extracted.is_err())
    return std::move(extracted).error();
  const std::filesystem::path go_root = tools_dir / "go";
  if (!std::filesystem::exists(go_root / "bin" / "go")) {
    return Error("goenv: toolchain archive has no go/bin/go");
  }
  Result<std::size_t> cleaned =
      remove_tree(paths.state_dir / "toolbox" / (archive.name + ".staging"));
  static_cast<void>(cleaned);
  return go_root;
}

Result<void> seed_module(
    const std::string& module, const std::string& version,
    const std::filesystem::path& module_cache,
    const std::vector<std::pair<std::string, std::string>>& url_to_sha,
    const Paths& paths, std::atomic<bool>* cancel) {
  ModuleFiles files;
  const std::string base =
      "https://proxy.golang.org/" + escape_module_path(module).value() + "/@v/";
  struct Want {
    const char* suffix;
    std::string* body;
  } wants[] = {
      {".info", &files.info}, {".mod", &files.mod}, {".zip", &files.zip}};

  for (const Want& want : wants) {
    const std::string url = base + version + want.suffix;
    std::string expected;
    bool pinned = false;
    for (const auto& [known_url, sha] : url_to_sha) {
      if (known_url == url) {
        expected = sha;
        pinned = true;
        break;
      }
    }
    if (!pinned) {
      return Error("goenv: no sha256 pin for " + url);
    }
    HttpGetOptions options;
    options.kind = RequestKind::kApi;
    options.purpose = "go-module";
    options.cancel = cancel;
    Result<HttpReply> reply = http_get(url, options, paths);
    if (reply.is_err())
      return std::move(reply).error();
    if (reply.value().status != 200) {
      return Error("goenv: " + url + " answered status " +
                   std::to_string(reply.value().status));
    }
    const std::filesystem::path staging =
        paths.state_dir / "goenv" / (module + "@" + version + want.suffix);
    Result<void> dir = ensure_dir(staging.parent_path());
    if (dir.is_err())
      return std::move(dir).error();
    Result<void> written =
        atomic_write(staging, reply.value().body, kPermPrivate);
    if (written.is_err())
      return std::move(written).error();
    Result<void> ok = verify_sha256_file(staging, expected);
    if (ok.is_err())
      return std::move(ok).error();
    *want.body = std::move(reply.value().body);
  }
  Result<std::filesystem::path> dir =
      write_module_cache_entry(module_cache, module, version, files);
  if (dir.is_err())
    return std::move(dir).error();
  return {};
}

std::vector<std::string> go_build_env(const std::filesystem::path& go_root,
                                      const std::filesystem::path& module_cache,
                                      const std::size_t max_procs) {
  return {
      "GOROOT=" + go_root.string(),
      "GOMODCACHE=" + module_cache.string(),
      "GOPROXY=file://" + module_cache.string() + "/cache/download",
      "GOFLAGS=-mod=mod",
      "GOSUMDB=off",
      "GONOSUMDB=*",
      "GOPRIVATE=*",
      "GOTOOLCHAIN=local",
      "GOPATH=" + (module_cache.parent_path() / "gopath").string(),
      "GOMAXPROCS=" + std::to_string(max_procs),
  };
}

}  // namespace berth