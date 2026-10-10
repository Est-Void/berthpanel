#pragma once

// src/berth/serve.h — the tiny HTTP/1.1 server shared by the two service
// paths: __serve-static (plan 5.1) and __serve-status (plan 5.4).
//
// Rules this file exists to hold (dev.md §6, §7):
//   * loopback only. is_loopback_bind() accepts 127.0.0.1 / ::1 /
//     localhost and NOTHING else; there is no flag or config that can turn
//     that into a wildcard. A public bind would be a new attack surface and
//     is refused before a socket exists.
//   * GET and HEAD only; every other verb is 405 with an Allow header. No
//     keep-alive, no HTTP/2: Connection: close on every reply.
//   * a static request NEVER escapes its root. Percent-decoding happens
//     once, NUL bytes are rejected, and the resolved path is compared to the
//     root by COMPONENTS after weakly_canonical — a symlink out is 403, and
//     a directory is never listed.
//   * every client is bounded (SO_RCVTIMEO/SO_SNDTIMEO) so a slowloris
//     cannot hold the accept loop, and the loop polls the cancel flag
//     between accepts (§5: every wait is interruptible).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/result.h"

namespace berth {

// A request head this long is refused with 413 before it is buffered
// without limit.
inline constexpr std::size_t kMaxRequestHeadBytes = std::size_t{8} * 1024;
// A GET/HEAD body is unusual; anything above this is refused with 413.
inline constexpr std::size_t kMaxRequestBodyBytes =
    std::size_t{1} * 1024 * 1024;
// One client, from connect to close. Slowloris is bounded by this.
inline constexpr std::chrono::milliseconds kClientTimeout{5000};

// A decoded request as the handler sees it. `path` has no query string and
// no percent escapes; `query` is the raw text after '?'.
struct HttpRequest {
  std::string method;  // as written; the server has already refused non-GET/HEAD
  std::string target;  // the raw request target
  std::string path;    // decoded, without query
  std::string query;
  std::string version;
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased

  [[nodiscard]] const std::string* header(std::string_view name) const;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> extra_headers;
};

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

struct ServerConfig {
  std::string listen_address = "127.0.0.1";
  std::uint16_t port = 0;  // 0 = the kernel picks one; on_ready reports it
  std::chrono::milliseconds client_timeout{kClientTimeout};
  std::size_t max_head_bytes = kMaxRequestHeadBytes;
  std::size_t max_body_bytes = kMaxRequestBodyBytes;
};

// The loopback-only bind policy. Ok(true) only for 127.0.0.1, ::1 and
// localhost (mapped to 127.0.0.1). Wildcards and every other address are
// Ok(false) — deliberately not configurable (plan 5.4).
[[nodiscard]] Result<bool> is_loopback_bind(const std::string& address);

// "127.0.0.1:8080" / "[::1]:8080" / "127.0.0.1:0" -> (address, port). Runs
// is_loopback_bind first, so a wildcard never reaches bind().
[[nodiscard]] Result<std::pair<std::string, std::uint16_t>> parse_listen_arg(
    std::string_view value);

// The reason phrase for the statuses this server emits.
[[nodiscard]] std::string_view http_reason(int status);

// The reply head only (no body). Connection: close and Content-Length are
// always present; extra headers are appended verbatim. PURE + testable.
[[nodiscard]] std::string render_http_head(
    int status, std::string_view content_type, std::size_t content_length,
    const std::vector<std::pair<std::string, std::string>>& extra = {});

// Builds the one-reply body for a plain error status.
[[nodiscard]] HttpResponse plain_reply(int status, std::string body);

// Runs the accept loop until *cancel is set. Binds loopback only, calls
// on_ready(actual_port) once after listen (so a :0 caller can print it), then
// serves one client at a time. Returns Err only on setup failure; a cancel is
// Ok. `cancel` must not be null.
[[nodiscard]] Result<void> run_http_server(
    const ServerConfig& config, const RequestHandler& handler,
    std::atomic<bool>* cancel,
    const std::function<void(std::uint16_t)>& on_ready = {});

// --- static handler (plan 5.1) ---

enum class StaticStatus { kOk, kNotFound, kForbidden };

struct StaticResolution {
  StaticStatus status = StaticStatus::kNotFound;
  std::filesystem::path path;
};

// PURE policy: decode `target` once (NUL rejected), drop the query, refuse a
// directory without an index, and prove the result stays under `root` by
// COMPONENTS after weakly_canonical. A symlink out is kForbidden.
[[nodiscard]] StaticResolution resolve_static_path(
    const std::filesystem::path& root, std::string_view target);

// Validates root is an existing directory, then returns the handler. A
// missing or non-directory root is a loud Err, never a 404-per-request.
[[nodiscard]] Result<RequestHandler> make_static_handler(
    std::filesystem::path root);

// Content-Type from a file extension; application/octet-stream when unknown.
[[nodiscard]] std::string_view content_type_for(std::string_view path);

}  // namespace berth
