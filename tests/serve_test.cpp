// Tests for serve.h (stages 5.1/5.4): the pure path policy, the loopback-only
// bind rules, and a real loopback server exercised with raw sockets. Every
// server test runs on 127.0.0.1:0 so it can never collide with another test.

#include "berth/serve.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "runner.h"
#include "test_env.h"

namespace {

namespace fs = std::filesystem;

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

// One raw request over a fresh blocking socket; the server answers with
// Connection: close, so reading to EOF is the whole reply.
std::string raw_request(const std::uint16_t port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    throw std::runtime_error("raw_request: socket");
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } closing{fd};

  timeval timeout{};
  timeout.tv_sec = 3;
  static_cast<void>(
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
  static_cast<void>(
      ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
      0)
    throw std::runtime_error("raw_request: connect");

  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n =
        ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (n <= 0)
      throw std::runtime_error("raw_request: send");
    sent += static_cast<std::size_t>(n);
  }
  ::shutdown(fd, SHUT_WR);

  std::string reply;
  char buffer[4096];
  while (true) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n < 0)
      break;
    if (n == 0)
      break;
    reply.append(buffer, static_cast<std::size_t>(n));
  }
  return reply;
}

int status_of(const std::string& wire) {
  const std::string prefix = "HTTP/1.1 ";
  if (wire.rfind(prefix, 0) != 0)
    return -1;
  return std::stoi(wire.substr(prefix.size(), 3));
}

std::string body_of(const std::string& wire) {
  const std::size_t split = wire.find("\r\n\r\n");
  if (split == std::string::npos)
    return "";
  return wire.substr(split + 4);
}

class TestServer {
 public:
  explicit TestServer(berth::RequestHandler handler) {
    berth::ServerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = 0;
    config.client_timeout = std::chrono::milliseconds(2000);
    thread_ = std::thread([this, config, handler]() {
      const berth::Result<void> ran = berth::run_http_server(
          config, handler, &cancel_, [this](const std::uint16_t port) {
            std::lock_guard<std::mutex> lock(mutex_);
            port_ = port;
            ready_ = true;
            cv_.notify_all();
          });
      static_cast<void>(ran);
    });
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::seconds(5), [this] { return ready_; });
  }

  ~TestServer() {
    cancel_.store(true);
    if (thread_.joinable())
      thread_.join();
  }

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }

 private:
  std::atomic<bool> cancel_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::uint16_t port_ = 0;
  bool ready_ = false;
};

berth::RequestHandler static_handler(const fs::path& root) {
  berth::Result<berth::RequestHandler> handler =
      berth::make_static_handler(root);
  if (handler.is_err())
    throw std::runtime_error(handler.error().ctx());
  return handler.value();
}

}  // namespace

TEST(serve_loopback_policy_refuses_wildcards) {
  CHECK(berth::is_loopback_bind("127.0.0.1").value());
  CHECK(berth::is_loopback_bind("::1").value());
  CHECK(berth::is_loopback_bind("localhost").value());
  CHECK(!berth::is_loopback_bind("0.0.0.0").value());
  CHECK(!berth::is_loopback_bind("::").value());
  CHECK(!berth::is_loopback_bind("192.168.1.10").value());
  CHECK(berth::is_loopback_bind("").is_err());
}

TEST(serve_parse_listen_rejects_non_loopback) {
  CHECK(berth::parse_listen_arg("127.0.0.1:0").is_ok());
  CHECK(berth::parse_listen_arg("127.0.0.1:8080").value().second == 8080);
  CHECK(berth::parse_listen_arg("[::1]:9000").value().second == 9000);
  CHECK(berth::parse_listen_arg("0.0.0.0:8080").is_err());
  CHECK(berth::parse_listen_arg("127.0.0.1:70000").is_err());
  CHECK(berth::parse_listen_arg("127.0.0.1:abc").is_err());
  CHECK(berth::parse_listen_arg("127.0.0.1").is_err());
}

TEST(serve_resolves_a_regular_file) {
  TempDir temp;
  write_text(temp.path() / "hello.txt", "hello");
  const berth::StaticResolution resolved =
      berth::resolve_static_path(temp.path(), "/hello.txt");
  CHECK(resolved.status == berth::StaticStatus::kOk);
  CHECK(fs::weakly_canonical(resolved.path) ==
        fs::weakly_canonical(temp.path() / "hello.txt"));
}

TEST(serve_rejects_traversal) {
  TempDir temp;
  write_text(temp.path() / "ok.txt", "ok");
  for (const std::string_view target :
       {"/../etc/passwd", "/%2e%2e/etc/passwd", "/..%2f..%2fetc%2fpasswd",
        "/a/../../etc/passwd"}) {
    const berth::StaticResolution resolved =
        berth::resolve_static_path(temp.path(), target);
    CHECK(resolved.status == berth::StaticStatus::kForbidden);
  }
}

TEST(serve_rejects_symlink_escape) {
  TempDir temp;
  write_text(temp.path() / "innocent.txt", "innocent");
  const fs::path link = temp.path() / "escape";
  std::error_code ec;
  fs::create_symlink("/etc/passwd", link, ec);
  CHECK(!ec);
  const berth::StaticResolution resolved =
      berth::resolve_static_path(temp.path(), "/escape");
  CHECK(resolved.status == berth::StaticStatus::kForbidden);
}

TEST(serve_rejects_null_byte_and_bad_escape) {
  TempDir temp;
  write_text(temp.path() / "ok.txt", "ok");
  CHECK(berth::resolve_static_path(temp.path(), "/a%00.txt").status ==
        berth::StaticStatus::kForbidden);
  CHECK(berth::resolve_static_path(temp.path(), "/%zz").status ==
        berth::StaticStatus::kForbidden);
}

TEST(serve_missing_file_is_not_found_and_directory_is_not_listed) {
  TempDir temp;
  std::error_code ec;
  fs::create_directory(temp.path() / "subdir", ec);
  CHECK(!ec);
  CHECK(berth::resolve_static_path(temp.path(), "/nope.txt").status ==
        berth::StaticStatus::kNotFound);
  CHECK(berth::resolve_static_path(temp.path(), "/subdir").status ==
        berth::StaticStatus::kForbidden);
}

TEST(serve_missing_root_is_a_loud_error) {
  TempDir temp;
  const fs::path missing = temp.path() / "does-not-exist";
  CHECK(berth::make_static_handler(missing).is_err());
}

TEST(serve_render_head_is_always_connection_close) {
  const std::string head = berth::render_http_head(404, "text/plain", 3);
  CHECK(head.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
  CHECK(head.find("Content-Length: 3\r\n") != std::string::npos);
  CHECK(head.find("Connection: close\r\n") != std::string::npos);
  CHECK(berth::http_reason(405) == "Method Not Allowed");
}

TEST(serve_static_server_answers_get_head_and_refusals) {
  TempDir temp;
  write_text(temp.path() / "index.html", "<h1>hi</h1>");
  write_text(temp.path() / "data.txt", "payload");
  TestServer server(static_handler(temp.path()));
  CHECK(server.port() >= 1024);

  const std::string get = raw_request(
      server.port(), "GET /data.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  CHECK(status_of(get) == 200);
  CHECK(body_of(get) == "payload");
  CHECK(get.find("Connection: close") != std::string::npos);

  const std::string index =
      raw_request(server.port(), "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(index) == 200);
  CHECK(body_of(index) == "<h1>hi</h1>");

  const std::string head = raw_request(
      server.port(), "HEAD /data.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  CHECK(status_of(head) == 200);
  CHECK(body_of(head).empty());
  CHECK(head.find("Content-Length: 7\r\n") != std::string::npos);

  const std::string missing =
      raw_request(server.port(), "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(missing) == 404);

  const std::string traversal = raw_request(
      server.port(), "GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(traversal) == 403);

  const std::string method =
      raw_request(server.port(), "POST /data.txt HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(method) == 405);
}
