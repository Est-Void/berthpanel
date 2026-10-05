#include "berth/httpclient.h"

#include <arpa/inet.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <psa/crypto.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include "../trust_store.h"
#include "../util/errno.h"
#include "../util/fd.h"
#include "../util/strutil.h"
#include "version.h"
#include "resolver.h"
namespace berth {

namespace {

constexpr std::string_view kRefusalPrefix = "refused: ";
constexpr std::string_view kTokenHost = "api.github.com";
constexpr int kPollSliceMs = 200;
constexpr int kRetryBackoffMs = 250;

[[nodiscard]] bool is_refusal(const Error& e) {
  return starts_with(e.root(), kRefusalPrefix);
}

[[nodiscard]] Error refusal(std::string why) {
  return Error(std::string(kRefusalPrefix) + std::move(why));
}

[[nodiscard]] bool is_digits(const std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](const char c) {
    return c >= '0' && c <= '9';
  });
}

[[nodiscard]] bool is_hex(const std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](const char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
  });
}

[[nodiscard]] std::string_view trim_ascii(std::string_view s) {
  return trim(s);
}

}  // namespace

Result<HttpUrl> parse_http_url(const std::string_view url) {
  if (url.empty()) return refusal("parse_http_url: empty url");
  for (const char c : url) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x21 || u == 0x7f) {
      return refusal("parse_http_url: control or space character in url");
    }
  }

  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return refusal("parse_http_url: missing scheme");
  }
  std::string scheme = std::string(url.substr(0, scheme_end));
  for (char& c : scheme) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  if (scheme != "https" && scheme != "http") {
    return refusal("parse_http_url: only https (and http for loopback "
                   "health checks) is supported");
  }

  std::string_view rest = url.substr(scheme_end + 3);
  const std::size_t path_start = rest.find('/');
  const std::string_view authority =
      path_start == std::string_view::npos ? rest : rest.substr(0, path_start);
  std::string path(path_start == std::string_view::npos ? "/"
                                                        : rest.substr(path_start));
  if (authority.empty()) return refusal("parse_http_url: empty host");

  // Userinfo is rejected: credentials never travel in URLs here.
  if (authority.find('@') != std::string_view::npos) {
    return refusal("parse_http_url: userinfo in url is not supported");
  }

  std::string host;
  std::string port;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      return refusal("parse_http_url: unterminated IPv6 literal");
    }
    host = std::string(authority.substr(1, close - 1));
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') {
        return refusal("parse_http_url: junk after IPv6 literal");
      }
      port = std::string(authority.substr(close + 2));
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      host = std::string(authority);
    } else {
      host = std::string(authority.substr(0, colon));
      port = std::string(authority.substr(colon + 1));
    }
  }

  for (const char c : host) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7f) {
      return refusal("parse_http_url: non-ascii or space in host");
    }
  }
  if (host.empty()) return refusal("parse_http_url: empty host");
  for (char& c : host) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }

  if (port.empty()) {
    port = scheme == "https" ? "443" : "80";
  } else {
    if (!is_digits(port) || port.size() > 5) {
      return refusal("parse_http_url: bad port '" + port + "'");
    }
    const long value = ::strtol(port.c_str(), nullptr, 10);
    if (value < 1 || value > 65535) {
      return refusal("parse_http_url: port out of range '" + port + "'");
    }
  }

  if (path.empty()) path = "/";
  return HttpUrl{std::move(scheme), std::move(host), std::move(port),
                 std::move(path)};
}

std::string join_redirect(const std::string_view current_url,
                          const std::string_view location) {
  if (location.empty()) return "";
  if (starts_with(location, "https://") || starts_with(location, "http://")) {
    return std::string(location);
  }
  if (starts_with(location, "//")) {
    const std::size_t scheme_end = current_url.find("://");
    if (scheme_end == std::string_view::npos) return "";
    return std::string(current_url.substr(0, scheme_end + 1)) +
           std::string(location);
  }
  if (location.front() == '/') {
    const std::size_t scheme_end = current_url.find("://");
    if (scheme_end == std::string_view::npos) return "";
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t path_start = current_url.find('/', authority_start);
    if (path_start == std::string_view::npos) {
      return std::string(current_url) + std::string(location);
    }
    return std::string(current_url.substr(0, path_start)) +
           std::string(location);
  }
  // Relative to the current document's directory.
  const std::size_t last_slash = current_url.rfind('/');
  if (last_slash == std::string_view::npos ||
      current_url.find("://") == std::string_view::npos) {
    return "";
  }
  return std::string(current_url.substr(0, last_slash + 1)) +
         std::string(location);
}

const std::string* HttpReply::header(const std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (key == name) return &value;
  }
  return nullptr;
}

namespace {

[[nodiscard]] std::string lower_copy(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

Result<std::pair<HttpReply, std::size_t>> parse_http_head(
    const std::string_view wire) {
  const std::size_t sep = wire.find("\r\n\r\n");
  if (sep == std::string_view::npos) {
    return Error("parse_http_head: incomplete head");
  }
  if (sep > kMaxHeadBytes) return Error("parse_http_head: head too large");

  const std::string_view head = wire.substr(0, sep);
  const std::size_t first_eol = head.find("\r\n");
  const std::string_view status_line =
      first_eol == std::string_view::npos ? head : head.substr(0, first_eol);

  if (!starts_with(status_line, "HTTP/")) {
    return Error("parse_http_head: bad status line");
  }
  const std::size_t sp1 = status_line.find(' ');
  if (sp1 == std::string_view::npos) {
    return Error("parse_http_head: bad status line");
  }
  const std::size_t sp2 = status_line.find(' ', sp1 + 1);
  const std::string_view code =
      sp2 == std::string_view::npos ? status_line.substr(sp1 + 1)
                                    : status_line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (!is_digits(code) || code.size() != 3) {
    return Error("parse_http_head: bad status code");
  }
  const int status = ::atoi(std::string(code).c_str());
  if (status < 100 || status > 599) {
    return Error("parse_http_head: status out of range");
  }

  HttpReply reply;
  reply.status = status;

  std::size_t pos = first_eol == std::string_view::npos ? head.size()
                                                        : first_eol + 2;
  while (pos < head.size()) {
    const std::size_t eol = head.find("\r\n", pos);
    const std::string_view line =
        head.substr(pos, (eol == std::string_view::npos ? head.size() : eol) -
                             pos);
    pos = eol == std::string_view::npos ? head.size() : eol + 2;
    if (line.empty()) continue;
    if (line.front() == ' ' || line.front() == '\t') {
      return Error("parse_http_head: obs-fold continuation is rejected");
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return Error("parse_http_head: bad header line");
    }
    reply.headers.emplace_back(
        lower_copy(trim_ascii(line.substr(0, colon))),
        std::string(trim_ascii(line.substr(colon + 1))));
  }
  return std::make_pair(std::move(reply), sep + 4);
}

BodyMode body_mode_of(const HttpReply& reply) {
  const std::string* te = reply.header("transfer-encoding");
  if (te != nullptr) {
    // Header values are case-insensitive (RFC 9110 §5.1).
    std::string value = *te;
    for (char& c : value) {
      c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    if (value.find("chunked") != std::string::npos) return BodyMode::kChunked;
  }
  if (content_length_of(reply).has_value()) return BodyMode::kContentLength;
  return BodyMode::kUntilClose;
}

std::optional<std::size_t> content_length_of(const HttpReply& reply) {
  // More than one content-length header is a smuggling shape: unusable.
  std::size_t count = 0;
  std::optional<std::size_t> value;
  for (const auto& [key, header_value] : reply.headers) {
    if (key != "content-length") continue;
    ++count;
    if (!is_digits(header_value)) continue;
    value = static_cast<std::size_t>(::strtoull(header_value.c_str(), nullptr,
                                                10));
  }
  if (count != 1) return std::nullopt;
  return value;
}

Result<std::optional<std::string>> decode_chunked(const std::string_view body,
                                                  const std::size_t max_bytes) {
  std::string out;
  std::size_t pos = 0;
  auto incomplete = [] { return std::optional<std::string>{}; };

  while (true) {
    const std::size_t eol = body.find("\r\n", pos);
    if (eol == std::string_view::npos) return incomplete();
    std::string_view size_line = body.substr(pos, eol - pos);
    const std::size_t semi = size_line.find(';');
    if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);
    size_line = trim_ascii(size_line);
    if (!is_hex(size_line)) {
      return Error("decode_chunked: bad chunk size");
    }
    const std::size_t size =
        static_cast<std::size_t>(::strtoull(std::string(size_line).c_str(),
                                            nullptr, 16));
    pos = eol + 2;

    if (size == 0) {
      // Optional trailers, terminated by a blank line.
      while (true) {
        const std::size_t e2 = body.find("\r\n", pos);
        if (e2 == std::string_view::npos) return incomplete();
        if (e2 == pos) return std::optional<std::string>(std::move(out));
        pos = e2 + 2;
      }
    }
    if (body.size() < pos + size) return incomplete();
    if (body.size() < pos + size + 2) return incomplete();
    out.append(body.substr(pos, size));
    if (out.size() > max_bytes) {
      return Error("decode_chunked: body exceeds cap");
    }
    pos += size;
    if (body.substr(pos, 2) != "\r\n") {
      if (body.size() < pos + 2) return incomplete();
      return Error("decode_chunked: bad chunk terminator");
    }
    pos += 2;
  }
}

bool response_is_complete(const std::string& wire, const std::size_t head_len,
                          const BodyMode mode,
                          const std::size_t content_length) {
  const std::string_view body = std::string_view(wire).substr(head_len);
  switch (mode) {
    case BodyMode::kContentLength:
      return body.size() >= content_length;
    case BodyMode::kChunked: {
      const Result<std::optional<std::string>> decoded =
          decode_chunked(body, kMaxBodyBytes);
      return decoded.is_ok() && decoded.value().has_value();
    }
    case BodyMode::kUntilClose:
      return false;
  }
  return false;
}

int effective_redirect_budget(const HttpGetOptions& options) {
  if (options.follow_allowlisted_redirects) return kMaxRedirectHops;
  return std::min(options.max_redirect_hops, kMaxRedirectHops);
}

RedirectDecision next_redirect(const HttpReply& reply,
                               const HttpGetOptions& options,
                               const std::string& current_url,
                               const int hops_done) {
  if (reply.status < 300 || reply.status > 399) {
    return {RedirectAction::kNotRedirect, ""};
  }
  const std::string* location = reply.header("location");
  if (location == nullptr || location->empty()) {
    return {RedirectAction::kStopBadLocation, ""};
  }
  const std::string target = join_redirect(current_url, *location);
  if (target.empty()) return {RedirectAction::kStopBadLocation, ""};
  const Result<HttpUrl> parsed = parse_http_url(target);
  if (parsed.is_err()) return {RedirectAction::kStopBadLocation, ""};

  const EndpointClass cls = classify_endpoint(parsed.value().host);
  if (!endpoint_permitted(cls, options.kind)) {
    return {RedirectAction::kStopDenied, target};
  }
  const int budget = effective_redirect_budget(options);
  if (hops_done >= budget) {
    return {RedirectAction::kStopCap, target};
  }
  return {RedirectAction::kFollow, target};
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::chrono::milliseconds remaining_ms(
    const std::chrono::steady_clock::time_point deadline) {
  const auto left = deadline - std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left);
  return ms > std::chrono::milliseconds::zero()
             ? ms
             : std::chrono::milliseconds::zero();
}

// Cancellable sleep in small slices.
void interruptible_sleep(const std::chrono::milliseconds total,
                         const std::atomic<bool>* cancel) {
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < total) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return;
    timespec ts{0, 50 * 1000 * 1000};
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
  }
}

[[nodiscard]] Result<int> wait_socket(const int fd, const short events,
                                      short* revents_out,
                                      const std::chrono::steady_clock::time_point deadline,
                                      const std::atomic<bool>* cancel,
                                      const std::string& what) {
  while (true) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error(what + ": cancelled");
    }
    const int left = static_cast<int>(
        std::min<std::chrono::milliseconds::rep>(remaining_ms(deadline).count(),
                                                 kPollSliceMs));
    if (left <= 0) return Error(what + ": timed out");
    pollfd p{fd, events, 0};
    const int rc = ::poll(&p, 1, left);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return Error(what + ": poll: " + errno_text(errno));
    }
    if (rc == 0) continue;
    if (revents_out != nullptr) *revents_out = p.revents;
    return 0;
  }
}

[[nodiscard]] Result<int> tcp_connect(const std::string& ip,
                                      const std::string& port,
                                      const std::chrono::steady_clock::time_point deadline,
                                      const std::atomic<bool>* cancel) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo* res = nullptr;
  const int rc = ::getaddrinfo(ip.c_str(), port.c_str(), &hints, &res);
  if (rc != 0) {
    return Error("tcp_connect: numeric resolve " + ip + ": " +
                 std::string(::gai_strerror(rc)));
  }
  const auto free_res = [&res] { ::freeaddrinfo(res); };

  std::string last;
  for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    FdGuard fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        ai->ai_protocol));
    if (fd.get() < 0) {
      last = errno_text(errno);
      continue;
    }
    const int c = ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
    if (c == 0) return fd.release();
    if (errno != EINPROGRESS) {
      last = errno_text(errno);
      continue;
    }
    short revents = 0;
    Result<int> waited =
        wait_socket(fd.get(), POLLOUT, &revents, deadline, cancel,
                    "tcp_connect " + ip);
    if (waited.is_err()) {
      last = std::move(waited).error().root();
      continue;
    }
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soerr, &len) != 0) {
      last = errno_text(errno);
      continue;
    }
    if (soerr == 0) return fd.release();
    last = errno_text(soerr);
  }
  free_res();
  return Error("tcp_connect: connect to " + ip + ":" + port + " failed: " + last);
}

// Per-request TLS contexts. Nothing here is global: sessions never share
// mbedTLS state (psa_crypto_init is the one global and is initialized
// once, below).
std::once_flag g_psa_init;

class TlsSession {
 public:
  explicit TlsSession(const int fd) : fd_(fd) {
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&drbg_);
    mbedtls_x509_crt_init(&cas_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_ssl_init(&ssl_);
  }

  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;

  ~TlsSession() {
    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&conf_);
    mbedtls_x509_crt_free(&cas_);
    mbedtls_ctr_drbg_free(&drbg_);
    mbedtls_entropy_free(&entropy_);
  }

  [[nodiscard]] Result<void> handshake(const std::string& hostname,
                                       const std::chrono::steady_clock::time_point deadline,
                                       const std::atomic<bool>* cancel) {
    std::call_once(g_psa_init, [] {
      // TLS 1.3 in mbedTLS 3.6 routes through PSA; the panel initializes
      // it once per process. Failure here is loud: no TLS, no request.
      if (::psa_crypto_init() != PSA_SUCCESS) {
        std::fputs("berth: fatal: psa_crypto_init failed\n", stderr);
        std::abort();
      }
    });

    int rc = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                   nullptr, 0);
    if (rc != 0) return tls_error("ctr_drbg_seed", rc);
    rc = mbedtls_x509_crt_parse(&cas_, kTrustStorePem, kTrustStorePemLen);
    if (rc != 0) return tls_error("trust store parse", rc);
    rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) return tls_error("config defaults", rc);
    mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf_, &cas_, nullptr);
    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
    rc = mbedtls_ssl_setup(&ssl_, &conf_);
    if (rc != 0) return tls_error("ssl_setup", rc);
    rc = mbedtls_ssl_set_hostname(&ssl_, hostname.c_str());
    if (rc != 0) return tls_error("set_hostname", rc);
    mbedtls_ssl_set_bio(&ssl_, this, bio_send, bio_recv, nullptr);

    while (true) {
      rc = mbedtls_ssl_handshake(&ssl_);
      if (rc == 0) return {};
      if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
          rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
        return tls_error("handshake " + hostname, rc);
      }
      Result<int> waited = wait_socket(
          fd_, rc == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, nullptr,
          deadline, cancel, "tls handshake");
      if (waited.is_err()) return std::move(waited).error();
    }
  }

  [[nodiscard]] Result<void> write_all(std::string_view data,
                                       const std::chrono::steady_clock::time_point deadline,
                                       const std::atomic<bool>* cancel) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const int rc = mbedtls_ssl_write(&ssl_,
                                       reinterpret_cast<const unsigned char*>(
                                           data.data() + sent),
                                       data.size() - sent);
      if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) {
        Result<int> waited = wait_socket(fd_, POLLIN | POLLOUT, nullptr,
                                         deadline, cancel, "tls write");
        if (waited.is_err()) return std::move(waited).error();
        continue;
      }
      if (rc <= 0) return tls_error("ssl_write", rc);
      sent += static_cast<std::size_t>(rc);
    }
    return {};
  }

  // Reads up to `buf_size` bytes; 0 signals TLS close notify (EOF).
  [[nodiscard]] Result<std::size_t> read_some(unsigned char* buf,
                                              const std::size_t buf_size,
                                              const std::chrono::steady_clock::time_point deadline,
                                              const std::atomic<bool>* cancel) {
    while (true) {
      const int rc = mbedtls_ssl_read(&ssl_, buf, buf_size);
      if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
        Result<int> waited = wait_socket(fd_, POLLIN, nullptr, deadline, cancel,
                                         "tls read");
        if (waited.is_err()) return std::move(waited).error();
        continue;
      }
      if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
        return std::size_t{0};
      }
      if (rc < 0) return tls_error("ssl_read", rc);
      return static_cast<std::size_t>(rc);
    }
  }

 private:
  static int bio_send(void* ctx, const unsigned char* buf, const std::size_t len) {
    auto* self = static_cast<TlsSession*>(ctx);
    const ssize_t n = ::send(self->fd_, buf, len, MSG_NOSIGNAL);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }

  static int bio_recv(void* ctx, unsigned char* buf, const std::size_t len) {
    auto* self = static_cast<TlsSession*>(ctx);
    const ssize_t n = ::recv(self->fd_, buf, len, 0);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }

  [[nodiscard]] static Error tls_error(const std::string& what, const int rc) {
    char buf[256];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return Error("tls: " + what + ": " + buf);
  }

  int fd_;
  mbedtls_entropy_context entropy_;
  mbedtls_ctr_drbg_context drbg_;
  mbedtls_x509_crt cas_;
  mbedtls_ssl_config conf_;
  mbedtls_ssl_context ssl_;
};

[[nodiscard]] Result<std::string> build_request(const HttpUrl& url,
                                                const HttpGetOptions& options) {
  std::string req = "GET " + url.path_query + " HTTP/1.1\r\nHost: " + url.host;
  const bool default_port =
      (url.scheme == "https" && url.port == "443") ||
      (url.scheme == "http" && url.port == "80");
  if (!default_port) req += ":" + url.port;
  req += "\r\n";
  req += "User-Agent: berthpanel/" BERTH_VERSION "\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: close\r\n";
  if (options.bearer_token != nullptr) {
    // §6.3: exact host match, never substring. A token bound for any other
    // host is a caller bug and is refused loudly.
    if (url.host != kTokenHost) {
      return refusal("bearer token is only permitted for api.github.com");
    }
    req += "Authorization: Bearer " + *options.bearer_token + "\r\n";
  }
  req += "\r\n";
  return req;
}

// One gated, resolved, connected, TLS'd HTTP exchange. No retries here.
[[nodiscard]] Result<HttpReply> get_once(const std::string& url,
                                         const HttpGetOptions& options,
                                         const Paths& paths) {
  const auto deadline =
      std::chrono::steady_clock::now() + options.per_attempt_timeout;

  Result<HttpUrl> parsed = parse_http_url(url);
  if (parsed.is_err()) return std::move(parsed).error();
  const HttpUrl target = parsed.value();

  // §6.2: the gate runs before every socket connect — including each hop.
  const EndpointClass cls = classify_endpoint(target.host);
  if (!endpoint_permitted(cls, options.kind)) {
    const NetlogEntry denied{
        .host = target.host,
        .ip = "",
        .port = static_cast<std::uint16_t>(::atoi(target.port.c_str())),
        .purpose = options.purpose,
        .note = cls == EndpointClass::kLoopbackOnly
                    ? "loopback requires health-check kind"
                    : "host not on allowlist",
        .allowed = false,
    };
    Result<void> logged = netlog_append(paths, denied);
    if (logged.is_err()) return std::move(logged).error();
    return refusal("host not permitted: " + target.host);
  }
  if (target.scheme == "http" &&
      !(cls == EndpointClass::kLoopbackOnly &&
        options.kind == RequestKind::kHealthCheck)) {
    return refusal("plaintext http is only permitted for loopback health "
                   "checks: " +
                   target.host);
  }

  std::vector<std::string> candidates;
  if (options.pinned_ip != nullptr) {
    candidates.push_back(*options.pinned_ip);
  } else {
    Result<std::vector<ResolvedIp>> resolved =
        resolve_host(target.host, remaining_ms(deadline), paths, options.cancel);
    if (resolved.is_err()) {
      Result<void> logged = netlog_append(paths, {
          .host = target.host,
          .ip = "",
          .port = static_cast<std::uint16_t>(::atoi(target.port.c_str())),
          .purpose = options.purpose,
          .note = "resolve failed: " + std::move(resolved).error().ctx(),
      });
      if (logged.is_err()) return std::move(logged).error();
      return Error("http_get: resolve " + target.host + " failed: " +
                   std::move(resolved).error().ctx());
    }
    std::vector<ResolvedIp> addresses = std::move(resolved).value();
    for (ResolvedIp& ip : addresses) {
      candidates.push_back(std::move(ip.ip));
    }
  }
  if (candidates.empty()) {
    return Error("http_get: no addresses for " + target.host);
  }

  Result<std::string> request = build_request(target, options);
  if (request.is_err()) return std::move(request).error();

  std::string last_err;
  for (const std::string& ip : candidates) {
    Result<int> connected = tcp_connect(ip, target.port, deadline, options.cancel);
    if (connected.is_err()) {
      last_err = std::move(connected).error().ctx();
      continue;
    }
    FdGuard fd(std::move(connected).value());

    TlsSession session(fd.get());
    if (target.scheme == "https") {
      Result<void> hs =
          session.handshake(target.host, deadline, options.cancel);
      if (hs.is_err()) {
        last_err = std::move(hs).error().ctx();
        continue;
      }
    }

    Result<void> sent = session.write_all(request.value(), deadline,
                                          options.cancel);
    if (sent.is_err()) {
      last_err = std::move(sent).error().ctx();
      continue;
    }

    std::string wire;
    bool eof = false;
    while (!eof) {
      if (wire.size() > options.max_body_bytes + kMaxHeadBytes) {
        return Error("http_get: response exceeds cap from " + target.host);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return Error("http_get: timed out reading response from " +
                     target.host);
      }
      unsigned char buf[16384];
      Result<std::size_t> chunk =
          session.read_some(buf, sizeof(buf), deadline, options.cancel);
      if (chunk.is_err()) {
        return std::move(chunk).error();
      }
      if (chunk.value() == 0) {
        eof = true;
        break;
      }
      wire.append(reinterpret_cast<const char*>(buf), chunk.value());

      const Result<std::pair<HttpReply, std::size_t>> head =
          parse_http_head(wire);
      if (head.is_err()) continue;  // head not complete yet
      const BodyMode mode = body_mode_of(head.value().first);
      const std::optional<std::size_t> content_length =
          content_length_of(head.value().first);
      if (mode == BodyMode::kContentLength && !content_length.has_value()) {
        return Error("http_get: unusable content-length from " + target.host);
      }
      if (response_is_complete(wire, head.value().second, mode,
                               content_length.value_or(0))) {
        break;
      }
    }

    Result<std::pair<HttpReply, std::size_t>> head = parse_http_head(wire);
    if (head.is_err()) {
      // Response ended before a complete head: transport-shaped failure.
      return Error("http_get: truncated response from " + target.host +
                   ": " + head.error().ctx());
    }
    HttpReply reply = std::move(head.value().first);
    const std::size_t head_len = head.value().second;
    std::string_view body = std::string_view(wire).substr(head_len);
    if (reply.status >= 300 && reply.status <= 399) {
      // Redirect bodies are irrelevant; drop them before the cap check.
      reply.body = "";
    } else {
      const BodyMode mode = body_mode_of(reply);
      if (mode == BodyMode::kChunked) {
        Result<std::optional<std::string>> decoded =
            decode_chunked(body, options.max_body_bytes);
        if (decoded.is_err()) return std::move(decoded).error();
        if (!decoded.value().has_value()) {
          return Error("http_get: truncated chunked body from " + target.host);
        }
        reply.body = std::move(decoded.value()).value();
      } else if (mode == BodyMode::kContentLength) {
        const std::size_t want = content_length_of(reply).value_or(0);
        if (body.size() < want) {
          return Error("http_get: truncated body from " + target.host);
        }
        body = body.substr(0, want);
        reply.body = std::string(body);
      } else {
        reply.body = std::string(body);
      }
    }

    Result<void> logged = netlog_append(paths, {
        .host = target.host,
        .ip = ip,
        .port = static_cast<std::uint16_t>(::atoi(target.port.c_str())),
        .purpose = options.purpose,
        .note = "status=" + std::to_string(reply.status),
    });
    if (logged.is_err()) return std::move(logged).error();
    return reply;
  }
  return Error("http_get: transport failed for " + target.host + ": " +
               last_err);
}

[[nodiscard]] Result<HttpReply> get_once_retried(const std::string& url,
                                                 const HttpGetOptions& options,
                                                 const Paths& paths) {
  const int attempts = std::max(1, options.max_attempts);
  std::string last_err;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (options.cancel != nullptr &&
        options.cancel->load(std::memory_order_relaxed)) {
      return Error("http_get: cancelled");
    }
    Result<HttpReply> reply = get_once(url, options, paths);
    if (reply.is_ok()) return reply;
    last_err = reply.error().ctx();
    if (is_refusal(reply.error())) return reply;  // refusals never retry
    if (attempt + 1 < attempts) {
      interruptible_sleep(std::chrono::milliseconds(kRetryBackoffMs),
                          options.cancel);
    }
  }
  return Error("http_get: giving up after " + std::to_string(attempts) +
               " attempts: " + last_err);
}

}  // namespace

Result<HttpReply> http_get(const std::string& url, const HttpGetOptions& options,
                           const Paths& paths) {
  if (options.max_redirect_hops > kMaxRedirectHops) {
    return Error("http_get: redirect budget exceeds the hard cap (" +
                 std::to_string(kMaxRedirectHops) + ")");
  }
  std::string current = url;
  int hops = 0;
  while (true) {
    Result<HttpReply> step = get_once_retried(current, options, paths);
    if (step.is_err()) return step;
    HttpReply reply = std::move(step).value();
    reply.hops = hops;

    const RedirectDecision decision = next_redirect(reply, options, current, hops);
    switch (decision.action) {
      case RedirectAction::kFollow:
        current = decision.next_url;
        ++hops;
        continue;
      case RedirectAction::kNotRedirect:
        return reply;
      case RedirectAction::kStopCap:
        reply.stop_note = "redirect hop cap reached";
        return reply;
      case RedirectAction::kStopDenied: {
        reply.stop_note = "redirect target refused by allowlist";
        // The refused hop is logged like any other attempt (§6.1).
        Result<HttpUrl> target = parse_http_url(decision.next_url);
        std::string host =
            target.is_ok() ? target.value().host : decision.next_url;
        Result<void> logged = netlog_append(paths, {
            .host = host,
            .ip = "",
            .port = 0,
            .purpose = options.purpose,
            .note = "redirect to non-allowlisted host refused",
            .allowed = false,
        });
        if (logged.is_err()) return std::move(logged).error();
        return reply;
      }
      case RedirectAction::kStopBadLocation:
        reply.stop_note = "missing or unresolvable Location";
        return reply;
    }
    return reply;
  }
}

}  // namespace berth
