// src/berth/net/doh.cpp — see doh.h for the contract.

#include "doh.h"

#include <arpa/inet.h>

#include <cstdio>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "client.h"

namespace berth {

namespace {

constexpr std::string_view kDohHost = "cloudflare-dns.com";
// Pinned anycast IPs serving DoH with a valid certificate for kDohHost.
constexpr std::string_view kDohBootstrapIps[] = {"1.1.1.1", "1.0.0.1"};

bool record_is_address(const nlohmann::json& item, ResolvedIp* out) {
  // The type check is `is_number_integer()` and not just `contains()`: a
  // record with "type":"A" (or null, or an array) makes get<int>() throw a
  // nlohmann type_error that only the parse_error handler above catches —
  // so a malformed-but-valid-JSON answer would abort the process instead of
  // failing as a Result (§6.10: no crash on malformed input).
  if (!item.is_object() || !item.contains("type") ||
      !item["type"].is_number_integer() || !item.contains("data") ||
      !item["data"].is_string()) {
    return false;
  }
  const int type = item["type"].get<int>();
  const std::string data = item["data"].get<std::string>();
  if (type == 1) {  // A
    in_addr v4{};
    if (::inet_pton(AF_INET, data.c_str(), &v4) == 1) {
      *out = {data, false};
      return true;
    }
    return false;
  }
  if (type == 28) {  // AAAA
    in6_addr v6{};
    if (::inet_pton(AF_INET6, data.c_str(), &v6) == 1) {
      *out = {data, true};
      return true;
    }
    return false;
  }
  // Other record types (CNAME chains and friends) are not addresses; a
  // response made only of those is a loud error below, not a skip.
  return false;
}

// The host goes into a URL, so everything outside the unreserved set is
// percent-encoded.
std::string percent_encode_host(const std::string& host) {
  std::string out;
  out.reserve(host.size());
  char buf[8];
  for (const char c : host) {
    const auto u = static_cast<unsigned char>(c);
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

// Returns the validated "Answer" array. Every rejection here is a loud
// error: a DoH response we do not understand is never read as "no records".
Result<nlohmann::json> validated_answers(const std::string_view body) {
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
  return doc["Answer"];
}

Result<std::vector<ResolvedIp>> resolve_doh(
    const std::string& host, const std::chrono::milliseconds budget,
    const Paths& paths, const std::atomic<bool>* cancel) {
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
  std::string last_error;
  for (const std::string_view bootstrap : kDohBootstrapIps) {
    const std::string ip(bootstrap);
    options.pinned_ip = &ip;
    Result<HttpReply> reply = http_get(url, options, paths, ResolveHost{});
    if (reply.is_err()) {
      last_error = reply.error().ctx();
      continue;
    }
    if (reply.value().status != 200) {
      last_error =
          "DoH answered status " + std::to_string(reply.value().status);
      continue;
    }
    return parse_doh_answers(reply.value().body);
  }
  return Error("resolver: DoH fallback failed for " + host + ": " + last_error);
}

}  // namespace

Result<std::vector<ResolvedIp>> parse_doh_answers(const std::string_view body) {
  Result<nlohmann::json> answers = validated_answers(body);
  if (answers.is_err())
    return std::move(answers).error();

  std::vector<ResolvedIp> out;
  for (const nlohmann::json& item : answers.value()) {
    ResolvedIp address;
    if (record_is_address(item, &address))
      out.push_back(address);
  }
  if (out.empty())
    return Error("doh: no valid A/AAAA records in Answer");
  return out;
}

DohFallback doh_fallback() {
  return [](const std::string& host, const std::chrono::milliseconds budget,
            const Paths& paths, const std::atomic<bool>* cancel) {
    return resolve_doh(host, budget, paths, cancel);
  };
}

}  // namespace berth
