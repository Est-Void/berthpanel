// Tests for httpclient pure parts (stages 3.4–3.5). No sockets here.
// The redirect tests DRIVE the loop decisions through the cap boundary —
// the exact test the old implementation faked (dev.md §11, §6.2).

#include "berth/httpclient.h"

#include <optional>
#include <string>

#include "runner.h"

using berth::body_mode_of;
using berth::BodyMode;
using berth::content_length_of;
using berth::decode_chunked;
using berth::HttpGetOptions;
using berth::HttpReply;
using berth::join_redirect;
using berth::next_redirect;
using berth::parse_http_head;
using berth::parse_http_url;
using berth::RedirectAction;
using berth::response_is_complete;

namespace {

HttpReply reply_with(const int status, const std::string& location = "") {
  HttpReply r;
  r.status = status;
  if (!location.empty())
    r.headers.emplace_back("location", location);
  return r;
}

}  // namespace

TEST(url_parse_full_and_defaults) {
  const auto r =
      parse_http_url("https://api.github.com/repos/x/y/tarball?ref=main");
  CHECK(r.is_ok());
  CHECK(r.value().scheme == "https");
  CHECK(r.value().host == "api.github.com");
  CHECK(r.value().port == "443");
  CHECK(r.value().path_query == "/repos/x/y/tarball?ref=main");

  const auto root = parse_http_url("https://github.com");
  CHECK(root.is_ok());
  CHECK(root.value().path_query == "/");
  CHECK(root.value().port == "443");

  const auto ported = parse_http_url("https://127.0.0.1:8443/health");
  CHECK(ported.is_ok());
  CHECK(ported.value().port == "8443");
  CHECK(ported.value().host == "127.0.0.1");
}

TEST(url_parse_normalizes_and_rejects) {
  const auto upper = parse_http_url("HTTPS://GitHub.COM/Path");
  CHECK(upper.is_ok());
  CHECK(upper.value().host == "github.com");

  const auto v6 = parse_http_url("https://[::1]:8080/x");
  CHECK(v6.is_ok());
  CHECK(v6.value().host == "::1");
  CHECK(v6.value().port == "8080");

  CHECK(parse_http_url("").is_err());
  CHECK(parse_http_url("ftp://github.com").is_err());
  CHECK(parse_http_url("github.com/no-scheme").is_err());
  CHECK(parse_http_url("https://user:pass@github.com/").is_err());  // §6.3
  CHECK(parse_http_url("https://[::1/x").is_err());
  CHECK(parse_http_url("https://host:0/").is_err());
  CHECK(parse_http_url("https://host:99999/").is_err());
  CHECK(parse_http_url("https://host:port/").is_err());
  CHECK(parse_http_url("https://ho st/").is_err());
}

TEST(join_redirect_forms) {
  const std::string current = "https://github.com/a/b/releases/tag/v1";
  CHECK(join_redirect(current, "https://example.com/x") ==
        "https://example.com/x");
  CHECK(join_redirect(current, "//github.com/a/b/download") ==
        "https://github.com/a/b/download");
  CHECK(join_redirect(current, "/a/b/download/1") ==
        "https://github.com/a/b/download/1");
  CHECK(join_redirect(current, "v2") ==
        "https://github.com/a/b/releases/tag/v2");
  CHECK(join_redirect(current, "").empty());
  CHECK(join_redirect("garbage", "/x").empty());
}

TEST(redirect_loop_exercises_cap_boundary_for_real) {
  // max_redirect_hops = 5 (also the hard cap). The loop must follow at
  // hops_done 0..4 and stop at exactly 5 — both sides of the boundary.
  const HttpReply redirect = reply_with(301, "https://github.com/next");
  for (int hops = 0; hops < 5; ++hops) {
    const auto d =
        next_redirect(redirect, HttpGetOptions{.max_redirect_hops = 5},
                      "https://github.com/start", hops);
    CHECK(d.action == RedirectAction::kFollow);
    CHECK(d.next_url == "https://github.com/next");
  }
  const auto capped =
      next_redirect(redirect, HttpGetOptions{.max_redirect_hops = 5},
                    "https://github.com/start", 5);
  CHECK(capped.action == RedirectAction::kStopCap);

  // A smaller requested budget stops earlier.
  const auto two =
      next_redirect(redirect, HttpGetOptions{.max_redirect_hops = 2},
                    "https://github.com/start", 2);
  CHECK(two.action == RedirectAction::kStopCap);

  // A caller requesting more than the hard cap is clamped to it: at 5
  // hops the loop stops even though the caller asked for 50.
  const auto greedy =
      next_redirect(redirect, HttpGetOptions{.max_redirect_hops = 50},
                    "https://github.com/start", 5);
  CHECK(greedy.action == RedirectAction::kStopCap);
  const auto greedy_at_4 =
      next_redirect(redirect, HttpGetOptions{.max_redirect_hops = 50},
                    "https://github.com/start", 4);
  CHECK(greedy_at_4.action == RedirectAction::kFollow);
}

TEST(redirect_target_denied_by_allowlist) {
  const auto denied = next_redirect(reply_with(302, "https://evil.io/steal"),
                                    HttpGetOptions{.max_redirect_hops = 5},
                                    "https://github.com/a", 0);
  CHECK(denied.action == RedirectAction::kStopDenied);

  // A near-miss host is denied exactly like a foreign one (§6.3).
  const auto near_miss = next_redirect(
      reply_with(302, "https://api.github.com.evil.io/x"),
      HttpGetOptions{.max_redirect_hops = 5}, "https://github.com/a", 0);
  CHECK(near_miss.action == RedirectAction::kStopDenied);

  // Loopback is not an API target.
  const auto loopback = next_redirect(
      reply_with(302, "http://127.0.0.1:8080/x"),
      HttpGetOptions{.kind = berth::RequestKind::kApi, .max_redirect_hops = 5},
      "https://github.com/a", 0);
  CHECK(loopback.action == RedirectAction::kStopDenied);
}

TEST(redirect_non_redirects_and_bad_locations) {
  const HttpGetOptions opts{.max_redirect_hops = 5};
  CHECK(next_redirect(reply_with(200), opts, "https://github.com/", 0).action ==
        RedirectAction::kNotRedirect);
  CHECK(next_redirect(reply_with(301), opts, "https://github.com/", 0).action ==
        RedirectAction::kStopBadLocation);
  CHECK(next_redirect(reply_with(301, ""), opts, "https://github.com/", 0)
            .action == RedirectAction::kStopBadLocation);
  // Scheme-only Location: no host, unresolvable.
  CHECK(
      next_redirect(reply_with(301, "http://"), opts, "https://github.com/", 0)
          .action == RedirectAction::kStopBadLocation);
}

TEST(head_parse_ok_and_rejects) {
  const std::string wire =
      "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
      "X-RateLimit-Remaining: 0\r\n\r\nbody";
  const auto r = parse_http_head(wire);
  CHECK(r.is_ok());
  CHECK(r.value().first.status == 403);
  CHECK(r.value().second == wire.size() - 4);
  const HttpReply& reply = r.value().first;
  CHECK(reply.header("content-type") != nullptr);
  CHECK(*reply.header("x-ratelimit-remaining") == "0");
  CHECK(reply.header("X-RateLimit-Remaining") == nullptr);  // names lowered

  CHECK(parse_http_head("HTTP/1.1 200 OK\r\n").is_err());  // incomplete
  CHECK(parse_http_head("garbage\r\n\r\n").is_err());
  CHECK(parse_http_head("HTTP/1.1 2ab OK\r\n\r\n").is_err());
  CHECK(parse_http_head("HTTP/1.1 999 X\r\n\r\n").is_err());
  CHECK(parse_http_head("HTTP/1.1 200 OK\r\n continued: y\r\n\r\n")
            .is_err());  // obs-fold rejected loudly
  CHECK(parse_http_head("HTTP/1.1 200 OK\r\nno-colon\r\n\r\n").is_err());
}

TEST(body_modes_and_content_length) {
  HttpReply chunked;
  chunked.headers.emplace_back("transfer-encoding", "Chunked");
  CHECK(body_mode_of(chunked) == BodyMode::kChunked);

  HttpReply sized;
  sized.headers.emplace_back("content-length", "12");
  CHECK(body_mode_of(sized) == BodyMode::kContentLength);
  CHECK(content_length_of(sized) == std::optional<std::size_t>(12));

  HttpReply neither;
  CHECK(body_mode_of(neither) == BodyMode::kUntilClose);
  CHECK(!content_length_of(neither).has_value());

  // Duplicate or non-numeric content-length is unusable (smuggling guard).
  HttpReply dup;
  dup.headers.emplace_back("content-length", "5");
  dup.headers.emplace_back("content-length", "6");
  CHECK(!content_length_of(dup).has_value());
  HttpReply junk;
  junk.headers.emplace_back("content-length", "5x");
  CHECK(!content_length_of(junk).has_value());
}

TEST(chunked_decode_full_incomplete_and_invalid) {
  const std::string full = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  const auto r = decode_chunked(full, 1024);
  CHECK(r.is_ok());
  CHECK(r.value().has_value());
  CHECK(r.value().value() == "Wikipedia");

  // Incomplete shapes return nullopt, never fabricated data.
  CHECK(!decode_chunked("4\r\nWik", 1024).value().has_value());
  CHECK(!decode_chunked("", 1024).value().has_value());
  CHECK(!decode_chunked("4\r\nWiki\r\n", 1024).value().has_value());

  CHECK(decode_chunked("zz\r\nx", 1024).is_err());
  CHECK(decode_chunked("4\r\nWikiXX", 1024).is_err());  // bad terminator
  CHECK(decode_chunked("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", 5).is_err());

  // Chunk extensions and trailers are tolerated.
  const std::string fancy = "4;name=v\r\nWiki\r\n0\r\nX-Trailer: 1\r\n\r\n";
  const auto f = decode_chunked(fancy, 1024);
  CHECK(f.is_ok());
  CHECK(f.value().has_value());
  CHECK(f.value().value() == "Wiki");
}

TEST(response_completeness) {
  const std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  const auto head = parse_http_head(wire);
  CHECK(head.is_ok());
  CHECK(response_is_complete(wire, head.value().second,
                             BodyMode::kContentLength, 5));
  CHECK(!response_is_complete(wire, head.value().second,
                              BodyMode::kContentLength, 6));
  CHECK(!response_is_complete(wire, head.value().second, BodyMode::kUntilClose,
                              0));
  const std::string chunked_wire =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: "
      "chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n";
  const auto chunked_head = parse_http_head(chunked_wire);
  CHECK(chunked_head.is_ok());
  CHECK(response_is_complete(chunked_wire, chunked_head.value().second,
                             BodyMode::kChunked, 0));
}

TEST(redirect_budget_is_zero_unless_explicitly_opted_in) {
  CHECK(berth::effective_redirect_budget(
            HttpGetOptions{.max_redirect_hops = 0}) == 0);
  CHECK(berth::effective_redirect_budget(
            HttpGetOptions{.max_redirect_hops = 3}) == 3);
  CHECK(berth::effective_redirect_budget(HttpGetOptions{
            .max_redirect_hops = 0, .follow_allowlisted_redirects = true}) ==
        berth::kMaxRedirectHops);
}

TEST(refusals_are_marked_and_never_confused_with_outages) {
  const auto bad = parse_http_url("https://user@host/");
  CHECK(bad.is_err());
  CHECK(bad.error().root().starts_with("refused: "));
}
