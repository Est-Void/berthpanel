#include "port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "../util/errno.h"

namespace berth {

Result<std::uint16_t> pick_free_port(const std::vector<std::uint16_t>& avoid) {
  constexpr int kMaxAttempts = 32;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return Error("pick_free_port: socket: " + errno_text(errno));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // kernel-assigned
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
        0) {
      const std::string why = errno_text(errno);
      ::close(fd);
      return Error("pick_free_port: bind 127.0.0.1:0: " + why);
    }
    sockaddr_in assigned{};
    socklen_t len = sizeof(assigned);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&assigned), &len) != 0) {
      const std::string why = errno_text(errno);
      ::close(fd);
      return Error("pick_free_port: getsockname: " + why);
    }
    const std::uint16_t port = ntohs(assigned.sin_port);
    ::close(fd);

    // Privileged ports cannot serve a user unit; avoid-listed ones collide.
    if (port < 1024)
      continue;
    const bool taken =
        std::find(avoid.begin(), avoid.end(), port) != avoid.end();
    if (taken)
      continue;
    return port;
  }
  return Error("pick_free_port: no free port after " +
               std::to_string(kMaxAttempts) + " attempts");
}

}
