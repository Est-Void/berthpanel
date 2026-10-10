// src/berth/net/client.cpp — see client.h for the contract.

#include "client.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <optional>
#include <string>
#include <utility>

#include "../../core/netlog.h"
#include "../../util/errno.h"
#include "../../util/fd.h"
#include "../../util/sleep.h"
#include "refusal.h"
#include "socket.h"
#include "tls.h"
#include "version.h"

namespace berth {

namespace {

constexpr std::string_view kApiHost = "api.github.com";
constexpr int kRetryBackoffMs = 250;
constexpr std::size_t kReadChunkBytes = 16384;

using Deadline = std::chrono::steady_clock::time_point;

struct Attempt {
  HttpUrl target;
  std::vector<std::string> candidates;
};

[[nodiscard]] bool cancelled(const HttpGetOptions& options) {
  return options.cancel != nullptr &&
         options.cancel->load(std::memory_order_relaxed);
}

// Netlog with a fresh attempt note; a log write failure is an Err the
// caller must see, never a swallowed warning.
Result<void> log_attempt(const Paths& paths, const HttpUrl& target,
                         const std::string& ip, const HttpGetOptions& options,
                         const std::string& note, const bool allowed) {
  return netlog_append(paths, NetlogEntry{.host = target.host,
                                          .ip = ip,
                                          .port = target.port_number(),
                                          .purpose = options.purpose,
                                          .note = note,
                                          .allowed = allowed});
}

// §6.2: the gate runs before every socket connect. The refusal is logged
// first, so net.log shows the attempt even though no byte ever moved.
Result<void> gate(const HttpUrl& target, const HttpGetOptions& options,
                  const Paths& paths, EndpointClass* cls_out) {
  const EndpointClass cls = classify_endpoint(target.host);
  if (cls_out != nullptr)
    *cls_out = cls;
  if (endpoint_permitted(cls, options.kind))
    return {};

  const std::string note = cls == EndpointClass::kLoopbackOnly
                               ? "loopback requires health-check kind"
                               : "host not on allowlist";
  Result<void> logged =
      log_attempt(paths, target, "", options, note, /*allowed=*/false);
  if (logged.is_err())
    return std::move(logged).error();
  return refusal("host not permitted: " + target.host);
}

// Plaintext http is refused unless it is a loopback health check, even for
// an allowlisted host: §6.1 allows the host, not the downgrade.
Result<void> gate_scheme(const HttpUrl& target, const EndpointClass cls,
                         const HttpGetOptions& options) {
  if (target.scheme != "http")
    return {};
  if (cls == EndpointClass::kLoopbackOnly &&
      options.kind == RequestKind::kHealthCheck) {
    return {};
  }
  return refusal(
      "plaintext http is only permitted for loopback health checks: " +
      target.host);
}

Result<std::vector<std::string>> candidate_ips(const HttpUrl& target,
                                               const HttpGetOptions& options,
                                               const Paths& paths,
                                               const Deadline deadline,
                                               const ResolveHost& resolve) {
  if (options.pinned_ip != nullptr) {
    return std::vector<std::string>{*options.pinned_ip};
  }
  if (!resolve) {
    return Error("http_get: " + target.host +
                 " needs resolution but no resolver was provided");
  }
  Result<std::vector<std::string>> resolved =
      resolve(target.host, remaining_ms(deadline), options.cancel, paths);
  if (resolved.is_ok())
    return resolved;

  const std::string why = resolved.error().ctx();
  Result<void> logged =
      log_attempt(paths, target, "", options, "resolve failed: " + why,
                  /*allowed=*/true);
  if (logged.is_err())
    return std::move(logged).error();
  return Error("http_get: resolve " + target.host + " failed: " + why);
}

Result<std::string> build_request(const HttpUrl& target,
                                  const HttpGetOptions& options) {
  std::string req =
      "GET " + target.path_query + " HTTP/1.1\r\nHost: " + target.host;
  const bool default_port =
      (target.scheme == "https" && target.port == "443") ||
      (target.scheme == "http" && target.port == "80");
  if (!default_port)
    req += ":" + target.port;
  req += "\r\n";
  req += "User-Agent: berthpanel/" BERTH_VERSION "\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: close\r\n";
  if (options.bearer_token != nullptr) {
    // §6.3: exact host match, never substring. A token bound for any other
    // host is a caller bug and is refused loudly.
    if (target.host != kApiHost) {
      return refusal("bearer token is only permitted for api.github.com");
    }
    req += "Authorization: Bearer " + *options.bearer_token + "\r\n";
  }
  // The blank line that terminates the head. It is required whether or not
  // a token was added: without it the peer waits for headers that never
  // come, and the request hangs until the deadline.
  req += "\r\n";
  return req;
}

enum class Framed {
  kIncomplete,
  kComplete,
  kUnusableContentLength,
};

// What the bytes accumulated so far say about framing. A head that does not
// parse yet is simply incomplete — more bytes may finish it.
Result<Framed> framing_of(const std::string& wire) {
  const Result<std::pair<HttpReply, std::size_t>> head = parse_http_head(wire);
  if (head.is_err())
    return Framed::kIncomplete;

  const BodyMode mode = body_mode_of(head.value().first);
  const std::optional<std::size_t> length =
      content_length_of(head.value().first);
  if (mode == BodyMode::kContentLength && !length.has_value()) {
    return Framed::kUnusableContentLength;
  }
  return response_is_complete(wire, head.value().second, mode,
                              length.value_or(0))
             ? Framed::kComplete
             : Framed::kIncomplete;
}

bool plain_response_is_complete(const std::string& wire) {
  const Result<Framed> state = framing_of(wire);
  return state.is_err() ? false : state.value() == Framed::kComplete;
}

// Reads until the response is complete under its own framing, or until the
// peer closes. Caps the accumulated bytes and the wall clock.
Result<void> read_response(TlsSession& session, const HttpUrl& target,
                           const HttpGetOptions& options,
                           const Deadline deadline, std::string* wire) {
  while (true) {
    if (wire->size() > options.max_body_bytes + kMaxHeadBytes) {
      return Error("http_get: response exceeds cap from " + target.host);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return Error("http_get: timed out reading response from " + target.host);
    }
    unsigned char buf[kReadChunkBytes];
    const Result<std::size_t> chunk =
        session.read_some(buf, sizeof(buf), deadline, options.cancel);
    if (chunk.is_err())
      return std::move(chunk).error();
    if (chunk.value() == 0)
      return {};
    wire->append(reinterpret_cast<const char*>(buf), chunk.value());

    const Result<Framed> state = framing_of(*wire);
    if (state.is_err())
      return std::move(state).error();
    if (state.value() == Framed::kUnusableContentLength) {
      return Error("http_get: unusable content-length from " + target.host);
    }
    if (state.value() == Framed::kComplete)
      return {};
  }
}

// A redirect body is irrelevant and is dropped before the cap check.
Result<void> attach_body(HttpReply* reply, const std::string& wire,
                         const std::size_t head_len,
                         const HttpGetOptions& options,
                         const std::string& host) {
  if (reply->status >= 300 && reply->status <= 399) {
    reply->body = "";
    return {};
  }
  const std::string_view body = std::string_view(wire).substr(head_len);
  const BodyMode mode = body_mode_of(*reply);
  if (mode == BodyMode::kChunked) {
    Result<std::optional<std::string>> decoded =
        decode_chunked(body, options.max_body_bytes);
    if (decoded.is_err())
      return std::move(decoded).error();
    if (!decoded.value().has_value()) {
      return Error("http_get: truncated chunked body from " + host);
    }
    reply->body = std::move(decoded.value()).value();
    return {};
  }
  if (mode == BodyMode::kContentLength) {
    const std::size_t want = content_length_of(*reply).value_or(0);
    if (body.size() < want) {
      return Error("http_get: truncated body from " + host);
    }
    reply->body = std::string(body.substr(0, want));
    return {};
  }
  reply->body = std::string(body);
  return {};
}

Result<HttpReply> parse_complete(const std::string& wire,
                                 const HttpGetOptions& options,
                                 const std::string& host) {
  Result<std::pair<HttpReply, std::size_t>> head = parse_http_head(wire);
  if (head.is_err()) {
    // Ended before a complete head: transport-shaped, not an HTTP result.
    return Error("http_get: truncated response from " + host + ": " +
                 head.error().ctx());
  }
  HttpReply reply = std::move(head.value()).first;
  const std::size_t head_len = head.value().second;
  const Result<void> body = attach_body(&reply, wire, head_len, options, host);
  if (body.is_err())
    return std::move(body).error();
  return reply;
}

// One attempt against one address: connect, TLS, send, read, parse.
// Plaintext bytes on the loopback health-check path. Separate from the TLS
// path on purpose: mbedTLS on a context that never completed a handshake
// fails with BAD_INPUT_PARAMETERS, so a single "session" abstraction over
// both would be a lie.
Result<void> read_plain_response(const int fd, const HttpUrl& target,
                                 const HttpGetOptions& options,
                                 const Deadline deadline, std::string* wire) {
  while (true) {
    if (wire->size() > options.max_body_bytes + kMaxHeadBytes) {
      return Error("http_get: response exceeds cap from " + target.host);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return Error("http_get: timed out reading response from " + target.host);
    }
    const Result<void> readable =
        wait_socket(fd, POLLIN, deadline, options.cancel, "plain read");
    if (readable.is_err())
      return readable;

    unsigned char buf[kReadChunkBytes];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return Error("plain read " + target.host + ": " + errno_text(errno));
    }
    if (n == 0)
      return {};  // peer closed
    wire->append(reinterpret_cast<const char*>(buf),
                 static_cast<std::size_t>(n));
    if (plain_response_is_complete(*wire))
      return {};
  }
}

Result<HttpReply> exchange_tls(const std::string& ip, const Attempt& attempt,
                               const std::string& request,
                               const HttpGetOptions& options,
                               const Deadline deadline) {
  const HttpUrl& target = attempt.target;
  Result<int> connected =
      tcp_connect(ip, target.port, deadline, options.cancel);
  if (connected.is_err())
    return std::move(connected).error();
  const FdGuard fd(connected.value());

  TlsSession session(fd.get());
  const Result<void> hs =
      session.handshake(target.host, deadline, options.cancel);
  if (hs.is_err())
    return std::move(hs).error();
  const Result<void> sent =
      session.write_all(request, deadline, options.cancel);
  if (sent.is_err())
    return std::move(sent).error();

  std::string wire;
  const Result<void> read =
      read_response(session, target, options, deadline, &wire);
  if (read.is_err())
    return std::move(read).error();
  return parse_complete(wire, options, target.host);
}

Result<HttpReply> exchange_plain(const std::string& ip, const Attempt& attempt,
                                 const std::string& request,
                                 const HttpGetOptions& options,
                                 const Deadline deadline) {
  const HttpUrl& target = attempt.target;
  Result<int> connected =
      tcp_connect(ip, target.port, deadline, options.cancel);
  if (connected.is_err())
    return std::move(connected).error();
  const FdGuard fd(connected.value());

  const Result<void> sent = write_all(fd.get(), request);
  if (sent.is_err())
    return std::move(sent).error();

  std::string wire;
  const Result<void> read =
      read_plain_response(fd.get(), target, options, deadline, &wire);
  if (read.is_err())
    return std::move(read).error();
  return parse_complete(wire, options, target.host);
}

Result<HttpReply> exchange_once(const std::string& ip, const Attempt& attempt,
                                const std::string& request,
                                const HttpGetOptions& options,
                                const Deadline deadline) {
  return attempt.target.scheme == "https"
             ? exchange_tls(ip, attempt, request, options, deadline)
             : exchange_plain(ip, attempt, request, options, deadline);
}

Result<HttpReply> get_once(const std::string& url,
                           const HttpGetOptions& options, const Paths& paths,
                           const ResolveHost& resolve) {
  const Deadline deadline =
      std::chrono::steady_clock::now() + options.per_attempt_timeout;

  Result<HttpUrl> parsed = parse_http_url(url);
  if (parsed.is_err())
    return std::move(parsed).error();

  Attempt attempt;
  attempt.target = std::move(parsed).value();

  EndpointClass cls = EndpointClass::kDenied;
  Result<void> gated = gate(attempt.target, options, paths, &cls);
  if (gated.is_err())
    return std::move(gated).error();
  const Result<void> scheme = gate_scheme(attempt.target, cls, options);
  if (scheme.is_err())
    return std::move(scheme).error();

  Result<std::string> request = build_request(attempt.target, options);
  if (request.is_err())
    return std::move(request).error();

  Result<std::vector<std::string>> candidates =
      candidate_ips(attempt.target, options, paths, deadline, resolve);
  if (candidates.is_err())
    return std::move(candidates).error();
  attempt.candidates = std::move(candidates).value();
  if (attempt.candidates.empty()) {
    return Error("http_get: no addresses for " + attempt.target.host);
  }

  std::string last_error;
  for (const std::string& ip : attempt.candidates) {
    Result<HttpReply> reply =
        exchange_once(ip, attempt, request.value(), options, deadline);
    if (reply.is_err()) {
      last_error = reply.error().ctx();
      continue;
    }
    Result<void> logged =
        log_attempt(paths, attempt.target, ip, options,
                    "status=" + std::to_string(reply.value().status),
                    /*allowed=*/true);
    if (logged.is_err())
      return std::move(logged).error();
    return reply;
  }
  return Error("http_get: transport failed for " + attempt.target.host + ": " +
               last_error);
}

Result<HttpReply> get_once_retried(const std::string& url,
                                   const HttpGetOptions& options,
                                   const Paths& paths,
                                   const ResolveHost& resolve) {
  const int attempts = std::max(1, options.max_attempts);
  std::string last_error;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (cancelled(options))
      return Error("http_get: cancelled");

    Result<HttpReply> reply = get_once(url, options, paths, resolve);
    if (reply.is_ok())
      return reply;
    last_error = reply.error().ctx();
    if (is_refusal(reply.error()))
      return reply;  // refusals never retry
    if (attempt + 1 < attempts) {
      static_cast<void>(interruptible_sleep(
          std::chrono::milliseconds(kRetryBackoffMs), options.cancel));
    }
  }
  return Error("http_get: giving up after " + std::to_string(attempts) +
               " attempts: " + last_error);
}

// Logs a redirect the allowlist refused (§6.1: a refused hop is still an
// attempt and must appear in net.log).
Result<void> log_refused_hop(const Paths& paths,
                             const RedirectDecision& decision,
                             const HttpGetOptions& options) {
  const Result<HttpUrl> target = parse_http_url(decision.next_url);
  const std::string host =
      target.is_ok() ? target.value().host : decision.next_url;
  return netlog_append(
      paths, NetlogEntry{.host = host,
                         .ip = "",
                         .port = 0,
                         .purpose = options.purpose,
                         .note = "redirect to non-allowlisted host refused",
                         .allowed = false});
}

}  // namespace

Result<HttpReply> http_get(const std::string& url,
                           const HttpGetOptions& options, const Paths& paths,
                           const ResolveHost& resolve) {
  if (options.max_redirect_hops > kMaxRedirectHops) {
    return Error("http_get: redirect budget exceeds the hard cap (" +
                 std::to_string(kMaxRedirectHops) + ")");
  }
  const int budget = effective_redirect_budget(
      options.max_redirect_hops, options.follow_allowlisted_redirects);
  std::string current = url;
  int hops = 0;
  while (true) {
    Result<HttpReply> step = get_once_retried(current, options, paths, resolve);
    if (step.is_err())
      return std::move(step).error();
    HttpReply reply = std::move(step).value();
    reply.hops = hops;

    const RedirectDecision decision =
        next_redirect(reply, options.kind, current, hops, budget);
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
        const Result<void> logged = log_refused_hop(paths, decision, options);
        if (logged.is_err())
          return std::move(logged).error();
        return reply;
      }
      case RedirectAction::kStopBadLocation:
        reply.stop_note = "missing or unresolvable Location";
        return reply;
    }
  }
}

}  // namespace berth
