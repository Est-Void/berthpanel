// src/berth/resolver.cpp — see resolver.h for the contract.

#include "resolver.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "../netlog.h"
#include "../util/result.h"
#include "httpclient.h"

namespace berth {

namespace {
constexpr std::string_view kDohHost = "cloudflare-dns.com";
// Pinned anycast IPs serving DoH with a valid certificate for kDohHost.
constexpr std::string_view kDohBootstrapIps[] = {"1.1.1.1", "1.0.0.1"};
constexpr std::chrono::milliseconds kSystemDnsBudget{5000};
constexpr std::chrono::milliseconds kPollSlice{50};
}  // namespace

Result<std::vector<ResolvedIp>> parse_doh_answers(const std::string_view body) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error& e) {
    return Error("doh: parse: " + std::string(e.what()));
  }
  if (!doc.is_object() || !doc.contains("Status") ||
      !doc["Status"].is_number_integer()) {
    return Error("doh: response has no Status");
  }
  if (doc["Status"].get<int>() != 0) {
    return Error("doh: resolver status " + doc["Status"].dump());
  }
  if (!doc.contains("Answer") || !doc["Answer"].is_array()) {
    return Error("doh: no Answer records");
  }
  std::vector<ResolvedIp> out;
  for (const nlohmann::json& item : doc["Answer"]) {
    if (!item.is_object() || !item.contains("type") ||
        !item.contains("data") || !item["data"].is_string()) {
      continue;
    }
    const int type = item["type"].get<int>();
    const std::string data = item["data"].get<std::string>();
    if (type == 1) {  // A
      in_addr v4{};
      if (::inet_pton(AF_INET, data.c_str(), &v4) == 1) {
        out.push_back({data, false});
      }
    } else if (type == 28) {  // AAAA
      in6_addr v6{};
      if (::inet_pton(AF_INET6, data.c_str(), &v6) == 1) {
        out.push_back({data, true});
      }
    }
    // Other record types (CNAME chains and friends) are not addresses;
    // a response made only of those is a loud error below, not a skip.
  }
  if (out.empty()) return Error("doh: no valid A/AAAA records in Answer");
  return out;
}

namespace {

Result<std::vector<ResolvedIp>> resolve_system(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
  if (rc != 0) {
    return Error("getaddrinfo " + host + ": " +
                 std::string(::gai_strerror(rc)));
  }
  std::vector<ResolvedIp> out;
  for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    char text[INET6_ADDRSTRLEN] = {0};
    bool ipv6 = false;
    if (ai->ai_family == AF_INET) {
      const auto* sa = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
      if (::inet_ntop(AF_INET, &sa->sin_addr, text, sizeof(text)) == nullptr) {
        continue;
      }
    } else if (ai->ai_family == AF_INET6) {
      const auto* sa = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
      if (::inet_ntop(AF_INET6, &sa->sin6_addr, text, sizeof(text)) == nullptr) {
        continue;
      }
      ipv6 = true;
    } else {
      continue;
    }
    const bool duplicate = std::any_of(
        out.begin(), out.end(),
        [&](const ResolvedIp& known) { return known.ip == text; });
    if (!duplicate) out.push_back({std::string(text), ipv6});
  }
  ::freeaddrinfo(res);
  if (out.empty()) {
    return Error("getaddrinfo " + host + ": no usable addresses");
  }
  return out;
}

// getaddrinfo has no cancellation; bound it from outside with a helper
// thread. On timeout the thread detaches (glibc caps its own resolver
// retries, so it always finishes eventually) and the caller's error is
// prompt — dev.md §5: every wait is interruptible.
Result<std::vector<ResolvedIp>> resolve_system_bounded(
    const std::string& host, const std::chrono::milliseconds budget,
    const std::atomic<bool>* cancel) {
  std::packaged_task<Result<std::vector<ResolvedIp>>(const std::string&)> task(
      [](const std::string& h) { return resolve_system(h); });
  std::future<Result<std::vector<ResolvedIp>>> future = task.get_future();
  std::thread runner(std::move(task), host);
  const auto t0 = std::chrono::steady_clock::now();
  while (true) {
    if (future.wait_for(kPollSlice) == std::future_status::ready) {
      runner.join();
      return future.get();
    }
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      runner.detach();
      return Error("resolver: cancelled while resolving " + host);
    }
    if (std::chrono::steady_clock::now() - t0 >= budget) {
      runner.detach();
      return Error("resolver: system DNS timed out after " +
                   std::to_string(budget.count()) + "ms for " + host);
    }
  }
}

std::string percent_encode_host(const std::string& host) {
  std::string out;
  out.reserve(host.size());
  char buf[8];
  for (const char c : host) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool safe = (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
                      u == '-' || u == '.';
    if (safe) {
      out += c;
    } else {
      ::snprintf(buf, sizeof(buf), "%%%02X", u);
      out += buf;
    }
  }
  return out;
}

Result<std::vector<ResolvedIp>> resolve_doh(
    const std::string& host, const std::chrono::milliseconds budget,
    const Paths& paths, std::atomic<bool>* cancel) {
  if (budget <= std::chrono::milliseconds{1000}) {
    return Error("resolver: no time budget left for the DoH fallback");
  }
  HttpGetOptions options;
  options.kind = RequestKind::kApi;
  options.purpose = "doh";
  options.max_attempts = 2;
  options.per_attempt_timeout = budget;
  options.cancel = cancel;

  const std::string url = std::string("https://") + std::string(kDohHost) +
                          "/dns-query?name=" + percent_encode_host(host) +
                          "&type=A";
  std::string last;
  for (const std::string_view bootstrap : kDohBootstrapIps) {
    const std::string ip(bootstrap);
    options.pinned_ip = &ip;
    Result<HttpReply> reply = http_get(url, options, paths);
    if (reply.is_err()) {
      last = reply.error().ctx();
      continue;
    }
    if (reply.value().status != 200) {
      last = "DoH answered status " + std::to_string(reply.value().status);
      continue;
    }
    return parse_doh_answers(reply.value().body);
  }
  return Error("resolver: DoH fallback failed for " + host + ": " + last);
}

}  // namespace

Result<std::vector<ResolvedIp>> resolve_host(
    const std::string& host, const std::chrono::milliseconds deadline,
    const Paths& paths, std::atomic<bool>* cancel) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto system_budget = std::min(deadline, kSystemDnsBudget);
  Result<std::vector<ResolvedIp>> system =
      resolve_system_bounded(host, system_budget, cancel);
  if (system.is_ok() && !system.value().empty()) {
    Result<void> logged = netlog_append(paths, {
        .host = host,
        .ip = "",
        .port = 0,
        .purpose = "resolver",
        .note = "system dns: " +
                std::to_string(system.value().size()) + " address(es)",
    });
    if (logged.is_err()) return std::move(logged).error();
    return system;
  }
  const std::string why = system.is_err() ? system.error().ctx() : "empty";
  Result<void> logged = netlog_append(paths, {
      .host = host,
      .ip = "",
      .port = 0,
      .purpose = "resolver",
      .note = "system dns failed (" + why + "); using DoH fallback",
  });
  if (logged.is_err()) return std::move(logged).error();

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);
  const auto remaining =
      deadline > elapsed ? deadline - elapsed : std::chrono::milliseconds{0};
  Result<std::vector<ResolvedIp>> doh =
      resolve_doh(host, remaining, paths, cancel);
  if (doh.is_err()) {
    return with_context(std::move(doh).error(), "resolver: " + host);
  }
  return doh;
}

}  // namespace berth
