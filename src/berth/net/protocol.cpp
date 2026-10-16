#include "protocol.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#include "../../util/ascii.h"
#include "../../util/strutil.h"

namespace berth {

namespace {

std::string lower_copy(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (is_upper_alpha(c))
      c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::optional<std::string_view> status_code_of(const std::string_view line) {
  if (!starts_with(line, "HTTP/"))
    return std::nullopt;
  const std::size_t first = line.find(' ');
  if (first == std::string_view::npos)
    return std::nullopt;
  const std::size_t second = line.find(' ', first + 1);
  const std::string_view code =
      second == std::string_view::npos
          ? line.substr(first + 1)
          : line.substr(first + 1, second - first - 1);
  if (code.size() != 3 || !all_chars(code, is_digit))
    return std::nullopt;
  return code;
}

Result<void> parse_status_line(const std::string_view line, HttpReply* reply) {
  const std::optional<std::string_view> code = status_code_of(line);
  if (!code.has_value())
    return Error("parse_http_head: bad status line");
  // Exactly three ASCII digits, so this cannot wrap (unlike atoi).
  const std::string_view digits = code.value();
  const int status =
      (digits[0] - '0') * 100 + (digits[1] - '0') * 10 + (digits[2] - '0');
  if (status < 100 || status > 599) {
    return Error("parse_http_head: status out of range");
  }
  reply->status = status;
  return {};
}

Result<void> parse_header_line(const std::string_view line, HttpReply* reply) {
  // obs-fold is rejected, not unwrapped: a folded header is a smuggling shape.
  if (line.front() == ' ' || line.front() == '\t') {
    return Error("parse_http_head: obs-fold continuation is rejected");
  }
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return Error("parse_http_head: bad header line");
  }
  reply->headers.emplace_back(lower_copy(trim(line.substr(0, colon))),
                              std::string(trim(line.substr(colon + 1))));
  return {};
}

Result<std::size_t> parse_headers(const std::string_view head,
                                  const std::size_t first_eol,
                                  HttpReply* reply) {
  std::size_t pos =
      first_eol == std::string_view::npos ? head.size() : first_eol + 2;
  while (pos < head.size()) {
    const std::size_t eol = head.find("\r\n", pos);
    const std::size_t end = eol == std::string_view::npos ? head.size() : eol;
    const std::string_view line = head.substr(pos, end - pos);
    pos = eol == std::string_view::npos ? head.size() : eol + 2;
    if (line.empty())
      continue;
    const Result<void> parsed = parse_header_line(line, reply);
    if (parsed.is_err())
      return std::move(parsed).error();
  }
  return std::size_t{0};
}

// Malformed and not-yet-there stay distinct (else a truncated body retries).
Result<std::optional<std::size_t>> parse_chunk_size(const std::string_view body,
                                                    std::size_t* pos) {
  const std::size_t eol = body.find("\r\n", *pos);
  if (eol == std::string_view::npos)
    return std::optional<std::size_t>{};
  std::string_view size_line = body.substr(*pos, eol - *pos);
  const std::size_t semi = size_line.find(';');
  if (semi != std::string_view::npos)
    size_line = size_line.substr(0, semi);
  size_line = trim(size_line);
  if (!all_chars(size_line, is_hex)) {
    return Error("decode_chunked: bad chunk size");
  }
  *pos = eol + 2;
  return std::optional<std::size_t>(static_cast<std::size_t>(
      ::strtoull(std::string(size_line).c_str(), nullptr, 16)));
}

Result<std::optional<std::string>> read_trailers(const std::string_view body,
                                                 std::size_t pos,
                                                 std::string out) {
  while (true) {
    const std::size_t eol = body.find("\r\n", pos);
    if (eol == std::string_view::npos) {
      return Error("decode_chunked: incomplete trailer block");
    }
    if (eol == pos)
      return std::optional<std::string>(std::move(out));
    pos = eol + 2;
  }
}

enum class ChunkStep {
  kContinue,
  kIncomplete,
};

// kIncomplete: the bytes are not here yet; Err: the framing is wrong.
Result<ChunkStep> append_chunk(const std::string_view body, std::size_t* pos,
                               const std::size_t size,
                               const std::size_t max_bytes, std::string* out) {
  if (body.size() < *pos + size + 2)
    return ChunkStep::kIncomplete;
  out->append(body.substr(*pos, size));
  if (out->size() > max_bytes) {
    return Error("decode_chunked: body exceeds cap");
  }
  *pos += size;
  if (body.substr(*pos, 2) != "\r\n") {
    return Error("decode_chunked: bad chunk terminator");
  }
  *pos += 2;
  return ChunkStep::kContinue;
}

}
const std::string* HttpReply::header(const std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (key == name)
      return &value;
  }
  return nullptr;
}

Result<std::pair<HttpReply, std::size_t>> parse_http_head(
    const std::string_view wire) {
  const std::size_t sep = wire.find("\r\n\r\n");
  if (sep == std::string_view::npos) {
    return Error("parse_http_head: incomplete head");
  }
  if (sep > kMaxHeadBytes)
    return Error("parse_http_head: head too large");

  const std::string_view head = wire.substr(0, sep);
  const std::size_t first_eol = head.find("\r\n");
  const std::string_view status_line =
      first_eol == std::string_view::npos ? head : head.substr(0, first_eol);

  HttpReply reply;
  const Result<void> status = parse_status_line(status_line, &reply);
  if (status.is_err())
    return std::move(status).error();
  const Result<std::size_t> parsed = parse_headers(head, first_eol, &reply);
  if (parsed.is_err())
    return std::move(parsed).error();
  return std::make_pair(std::move(reply), sep + 4);
}

BodyMode body_mode_of(const HttpReply& reply) {
  const std::string* te = reply.header("transfer-encoding");
  if (te != nullptr) {
    const std::string value = lower_copy(*te);
    if (value.find("chunked") != std::string::npos)
      return BodyMode::kChunked;
  }
  if (content_length_of(reply).has_value())
    return BodyMode::kContentLength;
  return BodyMode::kUntilClose;
}

std::optional<std::size_t> content_length_of(const HttpReply& reply) {
  std::size_t count = 0;
  std::optional<std::size_t> value;
  for (const auto& [key, header_value] : reply.headers) {
    if (key != "content-length")
      continue;
    ++count;
    if (header_value.empty() || !all_chars(header_value, is_digit))
      continue;
    value =
        static_cast<std::size_t>(::strtoull(header_value.c_str(), nullptr, 10));
  }
  if (count != 1)
    return std::nullopt;
  return value;
}

Result<std::optional<std::string>> decode_chunked(const std::string_view body,
                                                  const std::size_t max_bytes) {
  std::string out;
  std::size_t pos = 0;
  while (true) {
    Result<std::optional<std::size_t>> size = parse_chunk_size(body, &pos);
    if (size.is_err())
      return std::move(size).error();
    if (!size.value().has_value())
      return std::optional<std::string>{};
    if (size.value().value() == 0) {
      return read_trailers(body, pos, std::move(out));
    }

    Result<ChunkStep> step =
        append_chunk(body, &pos, size.value().value(), max_bytes, &out);
    if (step.is_err())
      return std::move(step).error();
    if (step.value() == ChunkStep::kIncomplete) {
      return std::optional<std::string>{};
    }
  }
}

bool response_is_complete(const std::string& wire, const std::size_t head_len,
                          const BodyMode mode,
                          const std::size_t content_length) {
  const std::string_view body = std::string_view(wire).substr(head_len);
  switch (mode) {
    case BodyMode::kContentLength:
      return body.size() >= content_length;
    case BodyMode::kChunked: {
      const Result<std::optional<std::string>> decoded =
          decode_chunked(body, kMaxBodyBytes);
      return decoded.is_ok() && decoded.value().has_value();
    }
    case BodyMode::kUntilClose:
      return false;
  }
  return false;
}

int effective_redirect_budget(const int max_redirect_hops,
                              const bool follow_allowlisted_redirects) {
  if (follow_allowlisted_redirects)
    return kMaxRedirectHops;
  return std::min(max_redirect_hops, kMaxRedirectHops);
}

RedirectDecision next_redirect(const HttpReply& reply, const RequestKind kind,
                               const std::string& current_url,
                               const int hops_done, const int hop_budget) {
  if (reply.status < 300 || reply.status > 399) {
    return {RedirectAction::kNotRedirect, ""};
  }
  const std::string* location = reply.header("location");
  if (location == nullptr || location->empty()) {
    return {RedirectAction::kStopBadLocation, ""};
  }
  const std::string target = join_redirect(current_url, *location);
  if (target.empty())
    return {RedirectAction::kStopBadLocation, ""};

  const Result<HttpUrl> parsed = parse_http_url(target);
  if (parsed.is_err())
    return {RedirectAction::kStopBadLocation, ""};
  if (!endpoint_permitted(classify_endpoint(parsed.value().host), kind)) {
    return {RedirectAction::kStopDenied, target};
  }
  if (hops_done >= hop_budget)
    return {RedirectAction::kStopCap, target};
  return {RedirectAction::kFollow, target};
}

}
