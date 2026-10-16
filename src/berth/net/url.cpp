#include "url.h"

#include <cstdlib>
#include <string>
#include <utility>

#include "../../util/ascii.h"
#include "refusal.h"

namespace berth {

namespace {

bool is_printable_ascii(const char c) {
  const auto u = static_cast<unsigned char>(c);
  return u > 0x20 && u < 0x7f;
}

std::string lower_copy(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (is_upper_alpha(c))
      c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

Result<std::pair<std::string, std::string>> split_authority(
    const std::string_view authority) {
  std::string host;
  std::string port;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      return refusal("parse_http_url: unterminated IPv6 literal");
    }
    host = std::string(authority.substr(1, close - 1));
    if (close + 1 >= authority.size())
      return std::make_pair(host, port);
    if (authority[close + 1] != ':') {
      return refusal("parse_http_url: junk after IPv6 literal");
    }
    port = std::string(authority.substr(close + 2));
    return std::make_pair(host, port);
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    return std::make_pair(std::string(authority), port);
  }
  host = std::string(authority.substr(0, colon));
  port = std::string(authority.substr(colon + 1));
  return std::make_pair(host, port);
}

Result<std::string> check_host(const std::string& host) {
  if (host.empty())
    return refusal("parse_http_url: empty host");
  for (const char c : host) {
    if (!is_printable_ascii(c)) {
      return refusal("parse_http_url: non-ascii or space in host");
    }
  }
  return lower_copy(host);
}

Result<std::string> check_port(const std::string& port,
                               const std::string& scheme) {
  if (port.empty())
    return std::string(scheme == "https" ? "443" : "80");
  if (port.size() > 5 || !all_chars(port, is_digit)) {
    return refusal("parse_http_url: bad port '" + port + "'");
  }
  const long value = ::strtol(port.c_str(), nullptr, 10);
  if (value < 1 || value > 65535) {
    return refusal("parse_http_url: port out of range '" + port + "'");
  }
  return port;
}

Result<std::pair<std::string, std::string_view>> split_scheme(
    const std::string_view url) {
  const std::size_t end = url.find("://");
  if (end == std::string_view::npos) {
    return refusal("parse_http_url: missing scheme");
  }
  const std::string scheme = lower_copy(url.substr(0, end));
  if (scheme != "https" && scheme != "http") {
    return refusal(
        "parse_http_url: only https (and http for loopback health "
        "checks) is supported");
  }
  return std::make_pair(scheme, url.substr(end + 3));
}

}
std::uint16_t HttpUrl::port_number() const {
  return static_cast<std::uint16_t>(::strtol(port.c_str(), nullptr, 10));
}

Result<HttpUrl> parse_http_url(const std::string_view url) {
  if (url.empty())
    return refusal("parse_http_url: empty url");
  for (const char c : url) {
    if (!is_printable_ascii(c)) {
      return refusal("parse_http_url: control or space character in url");
    }
  }

  const Result<std::pair<std::string, std::string_view>> split =
      split_scheme(url);
  if (split.is_err())
    return split.error();
  const std::string scheme = split.value().first;
  const std::string_view rest = split.value().second;

  const std::size_t path_start = rest.find_first_of("/?#");
  const std::string_view authority =
      path_start == std::string_view::npos ? rest : rest.substr(0, path_start);
  if (authority.empty())
    return refusal("parse_http_url: empty host");

  // Userinfo is rejected: credentials never travel in URLs here.
  if (authority.find('@') != std::string_view::npos) {
    return refusal("parse_http_url: userinfo in url is not supported");
  }

  const Result<std::pair<std::string, std::string>> host_port =
      split_authority(authority);
  if (host_port.is_err())
    return std::move(host_port).error();

  const Result<std::string> host = check_host(host_port.value().first);
  if (host.is_err())
    return std::move(host).error();
  const Result<std::string> port = check_port(host_port.value().second, scheme);
  if (port.is_err())
    return std::move(port).error();

  std::string path;
  if (path_start == std::string_view::npos) {
    path = "/";
  } else {
    path = std::string(rest.substr(path_start));
    // A query with no explicit path still needs a leading '/'.
    if (path.front() != '/')
      path.insert(path.begin(), '/');
  }
  if (path.empty())
    path = "/";

  return HttpUrl{scheme, host.value(), port.value(), std::move(path)};
}

std::string join_redirect(const std::string_view current_url,
                          const std::string_view location) {
  if (location.empty())
    return "";
  if (starts_with(location, "https://") || starts_with(location, "http://")) {
    return std::string(location);
  }
  const std::size_t scheme_end = current_url.find("://");
  if (scheme_end == std::string_view::npos)
    return "";

  if (starts_with(location, "//")) {
    return std::string(current_url.substr(0, scheme_end + 1)) +
           std::string(location);
  }
  if (location.front() == '/') {
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t path_start = current_url.find('/', authority_start);
    if (path_start == std::string_view::npos) {
      return std::string(current_url) + std::string(location);
    }
    return std::string(current_url.substr(0, path_start)) +
           std::string(location);
  }

  // RFC 3986 §5.3: the base path is kept, its query/fragment replaced.
  if (location.front() == '?' || location.front() == '#') {
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t cut = current_url.find_first_of("?#", authority_start);
    const std::size_t keep =
        cut == std::string_view::npos ? current_url.size() : cut;
    return std::string(current_url.substr(0, keep)) + std::string(location);
  }

  // A base with no path resolves against its root, not the scheme's "//".
  const std::size_t path_start = current_url.find('/', scheme_end + 3);
  if (path_start == std::string_view::npos) {
    return std::string(current_url) + "/" + std::string(location);
  }
  const std::size_t last_slash = current_url.rfind('/');
  if (last_slash == std::string_view::npos) {
    return "";
  }
  return std::string(current_url.substr(0, last_slash + 1)) +
         std::string(location);
}

}
