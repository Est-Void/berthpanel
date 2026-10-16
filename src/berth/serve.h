#pragma once

// loopback only: is_loopback_bind() accepts 127.0.0.1 / ::1 / localhost and
// nothing else, and no flag can turn that into a wildcard. GET and HEAD only;
// every other verb is 405 with an Allow header, Connection: close always.
// A static request never escapes its root: decode once, NUL rejected, compare
// the resolved path to the root COMPONENT-wise after weakly_canonical.
// Clients are time-bounded (SO_RCVTIMEO/SO_SNDTIMEO) so slowloris cannot hold
// the accept loop, which polls the cancel flag between accepts.

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

inline constexpr std::size_t kMaxRequestHeadBytes = std::size_t{8} * 1024;
inline constexpr std::size_t kMaxRequestBodyBytes =
    std::size_t{1} * 1024 * 1024;
// One client, from connect to close. Slowloris is bounded by this.
inline constexpr std::chrono::milliseconds kClientTimeout{5000};

// `path` has no query string and no percent escapes.
struct HttpRequest {
  std::string method;
  std::string target;  // the raw request target
  std::string path;
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

// Ok(true) only for 127.0.0.1, ::1 and localhost; not configurable.
[[nodiscard]] Result<bool> is_loopback_bind(const std::string& address);

// Runs is_loopback_bind first, so a wildcard never reaches bind().
[[nodiscard]] Result<std::pair<std::string, std::uint16_t>> parse_listen_arg(
    std::string_view value);

[[nodiscard]] std::string_view http_reason(int status);

[[nodiscard]] std::string render_http_head(
    int status, std::string_view content_type, std::size_t content_length,
    const std::vector<std::pair<std::string, std::string>>& extra = {});

[[nodiscard]] HttpResponse plain_reply(int status, std::string body);

// on_ready(actual_port) fires once after listen, so a :0 caller can print it.
[[nodiscard]] Result<void> run_http_server(
    const ServerConfig& config, const RequestHandler& handler,
    std::atomic<bool>* cancel,
    const std::function<void(std::uint16_t)>& on_ready = {});

enum class StaticStatus { kOk, kNotFound, kForbidden };

struct StaticResolution {
  StaticStatus status = StaticStatus::kNotFound;
  std::filesystem::path path;
};

// Decode once (NUL rejected), drop the query, refuse a directory with no index.
[[nodiscard]] StaticResolution resolve_static_path(
    const std::filesystem::path& root, std::string_view target);

[[nodiscard]] Result<RequestHandler> make_static_handler(
    std::filesystem::path root);

[[nodiscard]] std::string_view content_type_for(std::string_view path);

}
