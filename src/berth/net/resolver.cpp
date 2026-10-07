// src/berth/net/resolver.cpp — see resolver.h for the contract.

#include "resolver.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include "../../core/netlog.h"

namespace berth {

namespace {

constexpr std::chrono::milliseconds kSystemDnsBudget{5000};
constexpr std::chrono::milliseconds kPollSlice{50};

bool is_duplicate(const std::vector<ResolvedIp>& known, const std::string& ip) {
  return std::any_of(known.begin(), known.end(),
                     [&](const ResolvedIp& r) { return r.ip == ip; });
}

// Formats one addrinfo entry, or nullopt when it is not an A/AAAA address.
std::optional<std::string> address_text(const addrinfo* ai, bool* ipv6) {
  char text[INET6_ADDRSTRLEN] = {0};
  if (ai->ai_family == AF_INET) {
    const auto* sa = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
    if (::inet_ntop(AF_INET, &sa->sin_addr, text, sizeof(text)) == nullptr) {
      return std::nullopt;
    }
    *ipv6 = false;
  } else if (ai->ai_family == AF_INET6) {
    const auto* sa = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
    if (::inet_ntop(AF_INET6, &sa->sin6_addr, text, sizeof(text)) == nullptr) {
      return std::nullopt;
    }
    *ipv6 = true;
  } else {
    return std::nullopt;
  }
  return std::string(text);
}

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
    bool ipv6 = false;
    const std::optional<std::string> text = address_text(ai, &ipv6);
    if (!text.has_value())
      continue;
    if (is_duplicate(out, text.value()))
      continue;
    out.push_back({text.value(), ipv6});
  }
  ::freeaddrinfo(res);
  if (out.empty()) {
    return Error("getaddrinfo " + host + ": no usable addresses");
  }
  return out;
}

// getaddrinfo has no cancellation; bound it from outside with a helper
// thread. On timeout the thread detaches — the caller's error is prompt
// (dev.md §5: every wait is interruptible).
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

Result<void> log_resolver(const Paths& paths, const std::string& host,
                          const std::string& note) {
  return netlog_append(paths, NetlogEntry{.host = host,
                                          .ip = "",
                                          .port = 0,
                                          .purpose = "resolver",
                                          .note = note,
                                          .allowed = true});
}

}  // namespace

Result<std::vector<ResolvedIp>> resolve_host(
    const std::string& host, const std::chrono::milliseconds deadline,
    const Paths& paths, const std::atomic<bool>* cancel,
    const DohFallback& doh_fallback) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto system_budget = std::min(deadline, kSystemDnsBudget);
  Result<std::vector<ResolvedIp>> system =
      resolve_system_bounded(host, system_budget, cancel);
  if (system.is_ok() && !system.value().empty()) {
    Result<void> logged =
        log_resolver(paths, host,
                     "system dns: " + std::to_string(system.value().size()) +
                         " address(es)");
    if (logged.is_err())
      return std::move(logged).error();
    return system;
  }

  const std::string why = system.is_err() ? system.error().ctx() : "empty";
  Result<void> logged = log_resolver(
      paths, host, "system dns failed (" + why + "); using DoH fallback");
  if (logged.is_err())
    return std::move(logged).error();

  if (!doh_fallback) {
    return Error("resolver: system DNS failed for " + host + " (" + why +
                 ") and no DoH fallback is available");
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);
  const auto remaining =
      deadline > elapsed ? deadline - elapsed : std::chrono::milliseconds{0};
  Result<std::vector<ResolvedIp>> doh =
      doh_fallback(host, remaining, paths, cancel);
  if (doh.is_err()) {
    return with_context(std::move(doh).error(), "resolver: " + host);
  }
  return doh;
}

}  // namespace berth
