// src/netlog.cpp — the allowlist table and the net.log writer.
// See netlog.h for the contract. §6.1: table and test change together.

#include "netlog.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <string>

#include "atomic.h"
#include "util/errno.h"
#include "util/fd.h"

namespace berth {

namespace {

// The compile-time host allowlist (dev.md §6). Every addition or removal
// lands in the same commit as a matching change to
// tests/netlog_test.cpp (the table is pinned there exactly), with a
// public justification in the commit message.
constexpr std::array<std::string_view, 10> kAllowedEndpoints = {
    "api.github.com",
    "cdn.jsdelivr.net",
    "cloudflare-dns.com",
    "codeload.github.com",
    "dl.google.com",
    "github.com",
    "proxy.golang.org",
    "raw.githubusercontent.com",
    "release-assets.githubusercontent.com",
    "storage.googleapis.com",
};

constexpr std::array<std::string_view, 3> kLoopbackHosts = {
    "127.0.0.1",
    "::1",
    "localhost",
};

std::string normalize_host(std::string_view host) {
  std::string out;
  out.reserve(host.size());
  for (const char c : host) {
    if (c == '[' || c == ']') continue;  // IPv6 literal brackets
    out += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string one_line(std::string_view field) {
  std::string out;
  out.reserve(field.size());
  for (const char c : field) {
    out += (c == '\n' || c == '\r') ? ' ' : c;
  }
  return out;
}

std::string iso8601_utc_now() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  tm tm_utc{};
  ::gmtime_r(&ts.tv_sec, &tm_utc);
  char buf[48];
  const std::size_t n = ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S",
                                   &tm_utc);
  const int written = ::snprintf(buf + n, sizeof(buf) - n, ".%03ldZ",
                                 static_cast<long>(ts.tv_nsec / 1000000L));
  if (written <= 0 ||
      static_cast<std::size_t>(written) >= sizeof(buf) - n) {
    return "unknown-time";
  }
  return std::string(buf, n + static_cast<std::size_t>(written));
}

}  // namespace

std::vector<std::string_view> allowed_endpoints() {
  return {kAllowedEndpoints.begin(), kAllowedEndpoints.end()};
}

EndpointClass classify_endpoint(const std::string_view host) {
  if (host.empty()) return EndpointClass::kDenied;
  const std::string normalized = normalize_host(host);
  for (const std::string_view allowed : kAllowedEndpoints) {
    if (normalized == allowed) return EndpointClass::kAllowed;
  }
  for (const std::string_view loopback : kLoopbackHosts) {
    if (normalized == loopback) return EndpointClass::kLoopbackOnly;
  }
  return EndpointClass::kDenied;
}

bool endpoint_permitted(const EndpointClass cls, const RequestKind kind) {
  switch (cls) {
    case EndpointClass::kAllowed:
      return true;
    case EndpointClass::kLoopbackOnly:
      return kind == RequestKind::kHealthCheck;
    case EndpointClass::kDenied:
      return false;
  }
  return false;
}

Result<void> netlog_append(const Paths& paths, const NetlogEntry& entry) {
  std::string line = iso8601_utc_now();
  line += " ";
  line += one_line(entry.host);
  line += " ";
  line += one_line(entry.ip);
  line += " port=";
  line += std::to_string(entry.port);
  line += " purpose=";
  line += one_line(entry.purpose);
  line += entry.allowed ? " allowed" : " DENIED";
  if (!entry.note.empty()) {
    line += " note=\"";
    line += one_line(entry.note);
    line += "\"";
  }
  line += "\n";

  for (int attempt = 0; attempt < 2; ++attempt) {
    FdGuard fd(
        ::open(paths.net_log.c_str(),
               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (fd.get() >= 0) {
      std::size_t left = line.size();
      const char* cursor = line.data();
      while (left > 0) {
        const ssize_t n = ::write(fd.get(), cursor, left);
        if (n < 0) {
          if (errno == EINTR) continue;
          return Error("netlog: write: " + errno_text(errno));
        }
        cursor += n;
        left -= static_cast<std::size_t>(n);
      }
      return {};
    }
    if (errno != ENOENT || attempt == 1) {
      return Error("netlog: open " + paths.net_log.string() + ": " +
                   errno_text(errno));
    }
    // First failure because the state dir is absent: create it once.
    Result<void> dir = ensure_dir(paths.state_dir);
    if (dir.is_err()) return dir;
  }
  return Error("netlog: unreachable");
}

}  // namespace berth
