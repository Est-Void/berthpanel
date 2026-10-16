#include "socket.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <utility>

#include "../../util/errno.h"
#include "../../util/fd.h"

namespace berth {

namespace {

constexpr int kPollSliceMs = 200;

Result<void> connect_one(const addrinfo* ai, const std::string& ip,
                         const std::chrono::steady_clock::time_point deadline,
                         const std::atomic<bool>* cancel, FdGuard* fd_out,
                         std::string* last_error) {
  *fd_out = FdGuard(::socket(ai->ai_family,
                             ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             ai->ai_protocol));
  if (fd_out->get() < 0) {
    *last_error = errno_text(errno);
    return Error("socket: " + *last_error);
  }

  if (::connect(fd_out->get(), ai->ai_addr, ai->ai_addrlen) == 0)
    return {};

  if (errno != EINPROGRESS) {
    *last_error = errno_text(errno);
    return Error("connect: " + *last_error);
  }
  const Result<void> waited = wait_socket(fd_out->get(), POLLOUT, deadline,
                                          cancel, "tcp_connect " + ip);
  if (waited.is_err()) {
    *last_error = waited.error().root();
    return waited;
  }

  int soerr = 0;
  socklen_t len = sizeof(soerr);
  if (::getsockopt(fd_out->get(), SOL_SOCKET, SO_ERROR, &soerr, &len) != 0) {
    *last_error = errno_text(errno);
    return Error("getsockopt: " + *last_error);
  }
  if (soerr != 0) {
    *last_error = errno_text(soerr);
    return Error("connect: " + *last_error);
  }
  return {};
}

Result<addrinfo*> numeric_addrs(const std::string& ip,
                                const std::string& port) {
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
  return res;
}

}
std::chrono::milliseconds remaining_ms(
    const std::chrono::steady_clock::time_point deadline) {
  const auto left = deadline - std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left);
  return ms > std::chrono::milliseconds::zero()
             ? ms
             : std::chrono::milliseconds::zero();
}

Result<void> wait_socket(const int fd, const short events,
                         const std::chrono::steady_clock::time_point deadline,
                         const std::atomic<bool>* cancel,
                         const std::string& what) {
  while (true) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error(what + ": cancelled");
    }
    const auto left = remaining_ms(deadline);
    if (left <= std::chrono::milliseconds::zero()) {
      return Error(what + ": timed out");
    }
    const int wait_ms = static_cast<int>(
        std::min<std::chrono::milliseconds::rep>(left.count(), kPollSliceMs));
    pollfd p{fd, events, 0};
    const int rc = ::poll(&p, 1, wait_ms);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      return Error(what + ": poll: " + errno_text(errno));
    }
    if (rc == 0)
      continue;
    return {};
  }
}

Result<int> tcp_connect(const std::string& ip, const std::string& port,
                        const std::chrono::steady_clock::time_point deadline,
                        const std::atomic<bool>* cancel) {
  Result<addrinfo*> addresses = numeric_addrs(ip, port);
  if (addresses.is_err())
    return std::move(addresses).error();
  addrinfo* const res = addresses.value();

  std::string last_error;
  int connected_fd = -1;
  for (const addrinfo* ai = res; ai != nullptr && connected_fd < 0;
       ai = ai->ai_next) {
    FdGuard fd;
    const Result<void> connected =
        connect_one(ai, ip, deadline, cancel, &fd, &last_error);
    if (connected.is_ok())
      connected_fd = fd.release();
  }
  ::freeaddrinfo(res);

  if (connected_fd >= 0)
    return connected_fd;
  return Error("tcp_connect: connect to " + ip + ":" + port +
               " failed: " + last_error);
}

}
