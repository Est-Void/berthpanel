#include "serve.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "util/errno.h"
#include "util/fd.h"
#include "util/fs.h"

namespace berth {

namespace {

constexpr std::size_t kRecvChunk = 2048;
constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{64} * 1024 * 1024;
// Overflow guard for the Content-Length parse; must exceed the body bound.
constexpr unsigned long long kContentLengthParseCap = 1ull << 40;

int hex_digit(const char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string lower_ascii(const std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in)
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

std::string trim_ascii(const std::string_view in) {
  std::size_t begin = 0;
  std::size_t end = in.size();
  while (begin < end && (in[begin] == ' ' || in[begin] == '\t')) ++begin;
  while (end > begin && (in[end - 1] == ' ' || in[end - 1] == '\t')) --end;
  return std::string(in.substr(begin, end - begin));
}

// A dangling or non-hex escape is a refusal, not a guessed byte.
bool percent_decode(const std::string_view in, std::string* out) {
  out->clear();
  out->reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c != '%') {
      out->push_back(c);
      continue;
    }
    if (i + 2 >= in.size())
      return false;
    const int hi = hex_digit(in[i + 1]);
    const int lo = hex_digit(in[i + 2]);
    if (hi < 0 || lo < 0)
      return false;
    out->push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return true;
}

// "/tmp/root" must not match "/tmp/root-evil" (§11 prefix traps).
bool path_within(const std::filesystem::path& root,
                 const std::filesystem::path& candidate) {
  auto r = root.begin();
  auto c = candidate.begin();
  for (; r != root.end(); ++r, ++c) {
    if (c == candidate.end() || *r != *c)
      return false;
  }
  return true;
}

struct ParsedRequestHead {
  std::string method;
  std::string target;
  std::string version;
  std::vector<std::pair<std::string, std::string>> headers;
};

Result<ParsedRequestHead> parse_request_head(const std::string_view head) {
  std::vector<std::string_view> lines;
  std::size_t pos = 0;
  while (pos < head.size()) {
    const std::size_t nl = head.find("\r\n", pos);
    if (nl == std::string_view::npos)
      break;
    lines.push_back(head.substr(pos, nl - pos));
    pos = nl + 2;
    if (lines.back().empty())
      break;
  }
  if (lines.empty() || lines[0].empty())
    return Error("serve: empty request");

  const std::string_view request_line = lines[0];
  const std::size_t sp1 = request_line.find(' ');
  const std::size_t sp2 = request_line.rfind(' ');
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
      sp1 == sp2)
    return Error("serve: malformed request line");

  ParsedRequestHead parsed;
  parsed.method = std::string(request_line.substr(0, sp1));
  parsed.target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
  parsed.version = std::string(request_line.substr(sp2 + 1));
  if (parsed.method.empty() || parsed.target.empty())
    return Error("serve: malformed request line");

  for (std::size_t i = 1; i < lines.size(); ++i) {
    if (lines[i].empty())
      break;
    const std::size_t colon = lines[i].find(':');
    if (colon == std::string_view::npos)
      return Error("serve: malformed header line");
    parsed.headers.emplace_back(
        lower_ascii(trim_ascii(lines[i].substr(0, colon))),
        trim_ascii(lines[i].substr(colon + 1)));
  }
  return parsed;
}

const std::string* find_header(
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string_view name) {
  for (const auto& h : headers) {
    if (h.first == name)
      return &h.second;
  }
  return nullptr;
}

Result<std::size_t> content_length_of(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  std::size_t seen = 0;
  unsigned long long value = 0;
  for (const auto& h : headers) {
    if (h.first != "content-length")
      continue;
    ++seen;
    if (h.second.empty())
      return Error("serve: empty Content-Length");
    unsigned long long parsed = 0;
    for (const char c : h.second) {
      if (c < '0' || c > '9')
        return Error("serve: bad Content-Length");
      parsed = parsed * 10 + static_cast<unsigned long long>(c - '0');
      if (parsed > kContentLengthParseCap)
        return Error("serve: Content-Length too large");
    }
    value = parsed;
  }
  if (seen > 1)
    return Error("serve: duplicate Content-Length");
  return static_cast<std::size_t>(value);
}

void send_all(const int fd, const std::string_view data) {
  std::size_t left = data.size();
  const char* cursor = data.data();
  while (left > 0) {
    const ssize_t n = ::send(fd, cursor, left, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return;  // the peer is gone; nothing left to say
    }
    cursor += n;
    left -= static_cast<std::size_t>(n);
  }
}

void send_reply(const int fd, const HttpResponse& reply, const bool head_only) {
  const std::string head = render_http_head(
      reply.status, reply.content_type, reply.body.size(), reply.extra_headers);
  send_all(fd, head);
  if (!head_only)
    send_all(fd, reply.body);
}

struct ReadResult {
  bool ok = false;
  bool replied = false;  // an error reply was already sent
  ParsedRequestHead request;
};

// ok=false means drop the connection (EOF, timeout, cancel, replied).
ReadResult read_request(const int fd, const ServerConfig& config,
                        const std::atomic<bool>& cancel) {
  ReadResult result;
  std::string buffer;
  std::size_t head_end = std::string::npos;
  while (true) {
    if (cancel.load(std::memory_order_relaxed))
      return result;
    head_end = buffer.find("\r\n\r\n");
    if (head_end != std::string::npos)
      break;
    if (buffer.size() > config.max_head_bytes) {
      send_reply(fd, plain_reply(413, "payload too large\n"), false);
      result.replied = true;
      return result;
    }
    char chunk[kRecvChunk];
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return result;  // EAGAIN = timeout; the connection is dropped
    }
    if (n == 0)
      return result;
    buffer.append(chunk, static_cast<std::size_t>(n));
  }
  if (head_end + 4 > config.max_head_bytes) {
    send_reply(fd, plain_reply(413, "payload too large\n"), false);
    result.replied = true;
    return result;
  }

  const Result<ParsedRequestHead> parsed =
      parse_request_head(std::string_view(buffer).substr(0, head_end + 4));
  if (parsed.is_err()) {
    send_reply(fd, plain_reply(400, "bad request\n"), false);
    result.replied = true;
    return result;
  }
  result.request = parsed.value();

  if (find_header(result.request.headers, "transfer-encoding") != nullptr) {
    send_reply(fd, plain_reply(400, "bad request\n"), false);
    result.replied = true;
    return result;
  }
  const Result<std::size_t> declared =
      content_length_of(result.request.headers);
  if (declared.is_err()) {
    send_reply(fd, plain_reply(400, "bad request\n"), false);
    result.replied = true;
    return result;
  }
  if (declared.value() > config.max_body_bytes) {
    send_reply(fd, plain_reply(413, "payload too large\n"), false);
    result.replied = true;
    return result;
  }

  std::string body = buffer.substr(head_end + 4);
  while (body.size() < declared.value()) {
    if (cancel.load(std::memory_order_relaxed))
      return result;
    char chunk[kRecvChunk];
    const std::size_t want =
        std::min(sizeof(chunk), declared.value() - body.size());
    const ssize_t n = ::recv(fd, chunk, want, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return result;
    }
    if (n == 0)
      return result;
    body.append(chunk, static_cast<std::size_t>(n));
  }
  result.ok = true;
  return result;
}

HttpResponse static_reply(const std::filesystem::path& root,
                          const HttpRequest& request) {
  const StaticResolution resolved = resolve_static_path(root, request.target);
  switch (resolved.status) {
    case StaticStatus::kForbidden:
      return plain_reply(403, "forbidden\n");
    case StaticStatus::kNotFound:
      return plain_reply(404, "not found\n");
    case StaticStatus::kOk:
      break;
  }

  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(resolved.path, ec);
  if (ec || size > kMaxFileBytes)
    return plain_reply(403, "forbidden\n");

  const Result<std::string> data = read_file(resolved.path);
  if (data.is_err())
    return plain_reply(404, "not found\n");

  HttpResponse reply;
  reply.status = 200;
  reply.content_type = std::string(content_type_for(resolved.path.string()));
  reply.body = data.value();
  return reply;
}

}
const std::string* HttpRequest::header(const std::string_view name) const {
  for (const auto& h : headers) {
    if (h.first == name)
      return &h.second;
  }
  return nullptr;
}

Result<bool> is_loopback_bind(const std::string& address) {
  if (address.empty())
    return Error("serve: empty listen address");
  if (address == "127.0.0.1" || address == "::1" || address == "localhost")
    return true;
  return false;
}

Result<std::pair<std::string, std::uint16_t>> parse_listen_arg(
    const std::string_view value) {
  if (value.empty())
    return Error("serve: empty --listen");
  std::string address;
  std::string_view port_part;
  if (value.front() == '[') {
    const std::size_t close = value.find(']');
    if (close == std::string_view::npos || close + 1 >= value.size() ||
        value[close + 1] != ':')
      return Error("serve: malformed --listen: " + std::string(value));
    address = std::string(value.substr(1, close - 1));
    port_part = value.substr(close + 2);
  } else {
    const std::size_t colon = value.rfind(':');
    if (colon == std::string_view::npos)
      return Error("serve: --listen needs HOST:PORT");
    address = std::string(value.substr(0, colon));
    port_part = value.substr(colon + 1);
  }

  if (port_part.empty() || port_part.size() > 5)
    return Error("serve: bad port in --listen: " + std::string(value));
  std::uint32_t port = 0;
  for (const char c : port_part) {
    if (c < '0' || c > '9')
      return Error("serve: bad port in --listen: " + std::string(value));
    port = port * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (port > 65535)
    return Error("serve: port out of range in --listen: " + std::string(value));

  const Result<bool> loopback = is_loopback_bind(address);
  if (loopback.is_err())
    return std::move(loopback).error();
  if (!loopback.value()) {
    return Error("serve: refusing non-loopback bind '" + address +
                 "' (loopback only)");
  }
  return std::make_pair(address, static_cast<std::uint16_t>(port));
}

std::string_view http_reason(const int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 413:
      return "Content Too Large";
    case 500:
      return "Internal Server Error";
    default:
      return "Error";
  }
}

std::string render_http_head(
    const int status, const std::string_view content_type,
    const std::size_t content_length,
    const std::vector<std::pair<std::string, std::string>>& extra) {
  std::string out;
  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out += " ";
  out += http_reason(status);
  out += "\r\nContent-Type: ";
  out += content_type;
  out += "\r\nContent-Length: ";
  out += std::to_string(content_length);
  out += "\r\nConnection: close\r\n";
  for (const auto& h : extra) {
    out += h.first;
    out += ": ";
    out += h.second;
    out += "\r\n";
  }
  out += "\r\n";
  return out;
}

HttpResponse plain_reply(const int status, std::string body) {
  HttpResponse reply;
  reply.status = status;
  reply.body = std::move(body);
  return reply;
}

Result<void> run_http_server(
    const ServerConfig& config, const RequestHandler& handler,
    std::atomic<bool>* cancel,
    const std::function<void(std::uint16_t)>& on_ready) {
  if (cancel == nullptr)
    return Error("serve: null cancel flag");
  if (!handler)
    return Error("serve: null request handler");
  const Result<bool> loopback = is_loopback_bind(config.listen_address);
  if (loopback.is_err())
    return std::move(loopback).error();
  if (!loopback.value()) {
    return Error("serve: refusing non-loopback bind '" + config.listen_address +
                 "' (loopback only)");
  }
  const std::string address = config.listen_address == "localhost"
                                  ? std::string("127.0.0.1")
                                  : config.listen_address;
  const bool v6 = address == "::1";

  FdGuard listener(::socket(v6 ? AF_INET6 : AF_INET,
                            SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (listener.get() < 0)
    return Error("serve: socket: " + errno_text(errno));
  const int one = 1;
  static_cast<void>(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &one,
                                 sizeof(one)));

  int bind_rc = 0;
  if (v6) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = htons(config.port);
    bind_rc = ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr),
                     sizeof(addr));
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(config.port);
    bind_rc = ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr),
                     sizeof(addr));
  }
  if (bind_rc != 0) {
    return Error("serve: bind " + address + ":" + std::to_string(config.port) +
                 ": " + errno_text(errno));
  }
  if (::listen(listener.get(), 16) != 0)
    return Error("serve: listen: " + errno_text(errno));

  std::uint16_t bound_port;
  if (v6) {
    sockaddr_in6 actual{};
    socklen_t length = sizeof(actual);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&actual),
                      &length) != 0) {
      return Error("serve: getsockname: " + errno_text(errno));
    }
    bound_port = ntohs(actual.sin6_port);
  } else {
    sockaddr_in actual{};
    socklen_t length = sizeof(actual);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&actual),
                      &length) != 0) {
      return Error("serve: getsockname: " + errno_text(errno));
    }
    bound_port = ntohs(actual.sin_port);
  }
  if (on_ready)
    on_ready(bound_port);

  const long timeout_ms = static_cast<long>(config.client_timeout.count());
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

  while (!cancel->load(std::memory_order_relaxed)) {
    pollfd p{listener.get(), POLLIN, 0};
    const int ready = ::poll(&p, 1, 200);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return Error("serve: poll: " + errno_text(errno));
    }
    if (ready == 0)
      continue;

    const int client =
        ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == ECONNABORTED)
        continue;
      return Error("serve: accept: " + errno_text(errno));
    }
    FdGuard client_guard(client);
    static_cast<void>(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                   sizeof(timeout)));
    static_cast<void>(::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                   sizeof(timeout)));

    const ReadResult request = read_request(client, config, *cancel);
    if (!request.ok || request.replied)
      continue;
    if (request.request.method != "GET" && request.request.method != "HEAD") {
      send_reply(client, plain_reply(405, "method not allowed\n"), false);
      continue;
    }

    std::string target = request.request.target;
    std::string path = target;
    std::string query;
    const std::size_t question = target.find('?');
    if (question != std::string::npos) {
      path = target.substr(0, question);
      query = target.substr(question + 1);
    }
    std::string decoded;
    if (!percent_decode(path, &decoded) ||
        decoded.find('\0') != std::string::npos) {
      send_reply(client, plain_reply(400, "bad request\n"), false);
      continue;
    }

    HttpRequest http;
    http.method = request.request.method;
    http.target = std::move(target);
    http.path = std::move(decoded);
    http.query = std::move(query);
    http.version = request.request.version;
    http.headers = request.request.headers;

    const HttpResponse reply = handler(http);
    send_reply(client, reply, http.method == "HEAD");
  }
  return {};
}

StaticResolution resolve_static_path(const std::filesystem::path& root,
                                     const std::string_view target) {
  StaticResolution result;

  std::string_view path_part = target;
  const std::size_t question = path_part.find('?');
  if (question != std::string_view::npos)
    path_part = path_part.substr(0, question);

  std::string decoded;
  if (!percent_decode(path_part, &decoded))
    return {StaticStatus::kForbidden, {}};
  if (decoded.find('\0') != std::string::npos)
    return {StaticStatus::kForbidden, {}};
  if (decoded.empty() || decoded.front() != '/')
    return {StaticStatus::kForbidden, {}};

  const std::filesystem::path relative(decoded.substr(1));
  std::filesystem::path candidate = root / relative;
  const std::string filename = candidate.filename().string();
  if (filename == "." || filename == "..")
    return {StaticStatus::kForbidden, {}};

  // A missing path is DATA: only a different error is a refusal.
  std::error_code ec;
  const std::filesystem::file_status candidate_status =
      std::filesystem::status(candidate, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    return {StaticStatus::kForbidden, {}};
  ec.clear();
  const bool is_dir = std::filesystem::is_directory(candidate_status);

  std::filesystem::path index_candidate = candidate;
  if (is_dir) {
    index_candidate = candidate / "index.html";
    const std::filesystem::file_status index_status =
        std::filesystem::status(index_candidate, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
      return {StaticStatus::kForbidden, {}};
    ec.clear();
    if (!std::filesystem::is_regular_file(index_status)) {
      // No listing, ever: a directory without an index is refused.
      return {StaticStatus::kForbidden, {}};
    }
  }

  const std::filesystem::path root_canon =
      std::filesystem::weakly_canonical(root, ec);
  if (ec)
    return {StaticStatus::kForbidden, {}};
  const std::filesystem::path candidate_canon =
      std::filesystem::weakly_canonical(index_candidate, ec);
  if (ec)
    return {StaticStatus::kForbidden, {}};
  if (!path_within(root_canon, candidate_canon))
    return {StaticStatus::kForbidden, {}};

  const std::filesystem::file_status final_status =
      std::filesystem::status(candidate_canon, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    return {StaticStatus::kForbidden, {}};
  ec.clear();
  if (!std::filesystem::is_regular_file(final_status)) {
    if (std::filesystem::exists(final_status))
      return {StaticStatus::kForbidden, {}};
    return {StaticStatus::kNotFound, {}};
  }

  result.status = StaticStatus::kOk;
  result.path = candidate_canon;
  return result;
}

Result<RequestHandler> make_static_handler(std::filesystem::path root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec))
    return Error("serve: root is not a directory: " + root.string());
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(root, ec);
  if (ec)
    return Error("serve: cannot canonicalize root " + root.string() + ": " +
                 ec.message());
  return RequestHandler(
      [canonical](const HttpRequest& request) -> HttpResponse {
        return static_reply(canonical, request);
      });
}

std::string_view content_type_for(const std::string_view path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos)
    return "application/octet-stream";
  const std::string ext = lower_ascii(path.substr(dot));
  if (ext == ".html" || ext == ".htm")
    return "text/html; charset=utf-8";
  if (ext == ".css")
    return "text/css; charset=utf-8";
  if (ext == ".js" || ext == ".mjs")
    return "text/javascript; charset=utf-8";
  if (ext == ".json")
    return "application/json";
  if (ext == ".txt" || ext == ".md")
    return "text/plain; charset=utf-8";
  if (ext == ".svg")
    return "image/svg+xml";
  if (ext == ".png")
    return "image/png";
  if (ext == ".jpg" || ext == ".jpeg")
    return "image/jpeg";
  if (ext == ".gif")
    return "image/gif";
  if (ext == ".ico")
    return "image/x-icon";
  if (ext == ".woff2")
    return "font/woff2";
  if (ext == ".wasm")
    return "application/wasm";
  return "application/octet-stream";
}

}
