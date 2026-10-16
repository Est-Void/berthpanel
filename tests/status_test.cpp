// Tests for the read-only status page (plan 5.4): the pure renderers on a
// fake registry, a real loopback launch, and the loopback-only bind refusal.

#include "berth/status.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/jsonout.h"
#include "runner.h"

namespace {

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
    if (n <= 0)
      break;
    reply.append(buffer, static_cast<std::size_t>(n));
  }
  return reply;
}

int status_of(const std::string& wire) {
  if (wire.rfind("HTTP/1.1 ", 0) != 0)
    return -1;
  return std::stoi(wire.substr(9, 3));
}

std::string body_of(const std::string& wire) {
  const std::size_t split = wire.find("\r\n\r\n");
  return split == std::string::npos ? std::string() : wire.substr(split + 4);
}

class TestServer {
 public:
  explicit TestServer(berth::RequestHandler handler) {
    berth::ServerConfig config;
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

berth::StatusSnapshot fake_snapshot() {
  berth::StatusSnapshot snapshot;
  snapshot.services.push_back(berth::StatusService{
      "berthpanel-zeta", "Zeta", "z", "o/zeta", std::string(40, 'b'), 6002});
  snapshot.services.push_back(
      berth::StatusService{"berthpanel-alpha", "<script>alert(1)</script>", "a",
                           "o/alpha", std::string(40, 'a'), 6001});
  snapshot.ports.push_back(
      berth::StatusPort{"0.0.0.0", 6002, false, true, false});
  snapshot.ports.push_back(
      berth::StatusPort{"127.0.0.1", 6001, true, false, true});
  snapshot.open_deploys.push_back(
      berth::StatusDeploy{"berthpanel-gamma", "building", 123});
  return snapshot;
}

}
TEST(status_json_is_versioned_and_sorted) {
  const nlohmann::json doc = berth::status_json(fake_snapshot());
  CHECK(doc.at("schema").get<int>() == berth::kJsonSchema);
  CHECK(doc.at("command").get<std::string>() == "serve-status");
  CHECK(doc.at("services").at(0).at("service_name") == "berthpanel-alpha");
  CHECK(doc.at("services").at(1).at("service_name") == "berthpanel-zeta");
  CHECK(doc.at("ports").at(0).at("address") == "0.0.0.0");
  CHECK(doc.at("ports").at(0).at("wildcard").get<bool>());
  CHECK(doc.at("ports").at(0).at("owned").get<bool>() == false);
  CHECK(doc.at("ports").at(1).at("owned").get<bool>());
  CHECK(doc.at("open_deploys").at(0).at("service_name") == "berthpanel-gamma");
}

TEST(status_json_reports_a_section_error_loudly) {
  berth::StatusSnapshot snapshot;
  snapshot.error = "registry: corrupt state.json";
  const nlohmann::json doc = berth::status_json(snapshot);
  CHECK(doc.at("error").get<std::string>().find("registry") !=
        std::string::npos);
}

TEST(status_html_escapes_user_values) {
  const std::string html = berth::render_status_html(fake_snapshot());
  CHECK(html.find("<script>") == std::string::npos);
  CHECK(html.find("&lt;script&gt;") != std::string::npos);
  CHECK(html.find("berthpanel-alpha") != std::string::npos);
  CHECK(html.find("127.0.0.1") != std::string::npos);
  CHECK(html.find("loopback only") != std::string::npos);
}

TEST(status_loopback_only_bind_is_refused_elsewhere) {
  CHECK(berth::parse_listen_arg("0.0.0.0:8080").is_err());
  CHECK(berth::parse_listen_arg("192.168.0.2:8080").is_err());
  CHECK(berth::parse_listen_arg("127.0.0.1:8080").is_ok());
}

TEST(status_server_serves_html_and_json_and_nothing_else) {
  TestServer server(berth::make_status_handler(fake_snapshot));
  CHECK(server.port() >= 1024);

  const std::string html =
      raw_request(server.port(), "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(html) == 200);
  CHECK(html.find("text/html") != std::string::npos);
  CHECK(body_of(html).find("berthpanel-zeta") != std::string::npos);

  const std::string json = raw_request(
      server.port(), "GET /status.json HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(json) == 200);
  CHECK(json.find("application/json") != std::string::npos);
  const nlohmann::json doc = nlohmann::json::parse(body_of(json));
  CHECK(doc.at("services").size() == 2);

  const std::string missing =
      raw_request(server.port(), "GET /admin HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(status_of(missing) == 404);
}
