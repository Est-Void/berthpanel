#include "secrets.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "../core/atomic.h"
#include "../core/names.h"
#include "../util/ascii.h"
#include "../util/fs.h"

namespace berth {

namespace {

[[nodiscard]] bool valid_key(const std::string& key) {
  return !key.empty() && is_env_key_first(key.front()) &&
         all_chars(std::string_view(key).substr(1), is_env_key_char);
}

// Always quoted: unquoted stops at the first space or '#' and truncates.
[[nodiscard]] Result<std::string> quote_env_value(const std::string& value) {
  for (const char c : value) {
    if (c == '\n' || c == '\r' || c == '\0') {
      return Error(
          "secrets: value contains a newline or NUL, which an "
          "EnvironmentFile cannot represent");
    }
  }
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') {
      out += '\\';
    }
    out += c;
  }
  out += "\"";
  return out;
}

}
Result<std::string> render_environment_file(
    const std::vector<SecretEntry>& entries) {
  std::string out;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::string& key = entries[i].first;
    const std::string& value = entries[i].second;
    if (!valid_key(key)) {
      return Error("secrets: invalid environment key: '" + key + "'");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (entries[j].first == key) {
        return Error("secrets: duplicate environment key: '" + key + "'");
      }
    }
    Result<std::string> quoted = quote_env_value(value);
    if (quoted.is_err())
      return std::move(quoted).error();
    out += key;
    out += "=";
    out += std::move(quoted).value();
    out += "\n";
  }
  return out;
}

Result<std::filesystem::path> write_secrets(
    const Paths& paths, const std::string& service_name,
    const std::vector<SecretEntry>& entries) {
  if (!validate_unit_name(service_name)) {
    return Error("secrets: invalid service name: '" + service_name + "'");
  }
  Result<std::string> text = render_environment_file(entries);
  if (text.is_err())
    return std::move(text).error();
  Result<void> dir = ensure_dir(paths.secrets_dir);
  if (dir.is_err())
    return std::move(dir).error();
  const std::filesystem::path target =
      paths.secrets_dir / (service_name + ".env");
  Result<void> written = atomic_write(target, text.value(), kPermPrivate);
  if (written.is_err())
    return std::move(written).error();
  return target;
}

namespace {

// The key is everything before '=', with surrounding blanks ignored. A comment
// line yields a key that fails valid_key(), so it is never mistaken for one.
bool parse_line_key(const std::string_view line, std::string* key) {
  const std::size_t eq = line.find('=');
  if (eq == std::string_view::npos)
    return false;
  std::string_view candidate = line.substr(0, eq);
  std::size_t begin = 0;
  while (begin < candidate.size() &&
         (candidate[begin] == ' ' || candidate[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = candidate.size();
  while (end > begin &&
         (candidate[end - 1] == ' ' || candidate[end - 1] == '\t')) {
    --end;
  }
  if (end == begin)
    return false;
  *key = std::string(candidate.substr(begin, end - begin));
  return true;
}

// A duplicate key line is corruption, not last-one-wins: refuse before writing.
Result<std::string> replace_or_append_line(std::string text,
                                           const std::string& key,
                                           const std::string& quoted) {
  std::size_t start = 0;
  std::size_t found = std::string::npos;
  while (start <= text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end =
        newline == std::string::npos ? text.size() : newline;
    std::string line_key;
    const std::string_view line(text.data() + start, end - start);
    if (parse_line_key(line, &line_key) && line_key == key) {
      if (found != std::string::npos) {
        return Error("secrets: duplicate key '" + key +
                     "' in the environment file");
      }
      found = start;
    }
    if (newline == std::string::npos)
      break;
    start = newline + 1;
  }
  std::string line = key;
  line += "=";
  line += quoted;
  if (found != std::string::npos) {
    const std::size_t newline = text.find('\n', found);
    std::string out = text.substr(0, found);
    out += line;
    if (newline != std::string::npos)
      out += text.substr(newline);
    return out;
  }
  if (!text.empty() && text.back() != '\n')
    text += '\n';
  text += line;
  text += '\n';
  return text;
}

}

Result<std::filesystem::path> set_environment_value(
    const Paths& paths, const std::string& service_name, const std::string& key,
    const std::string& value) {
  if (!validate_unit_name(service_name)) {
    return Error("secrets: invalid service name: '" + service_name + "'");
  }
  if (!valid_key(key)) {
    return Error("secrets: invalid environment key: '" + key + "'");
  }
  Result<std::string> quoted = quote_env_value(value);
  if (quoted.is_err())
    return std::move(quoted).error();
  Result<void> dir = ensure_dir(paths.secrets_dir);
  if (dir.is_err())
    return std::move(dir).error();

  const std::filesystem::path target =
      paths.secrets_dir / (service_name + ".env");
  std::string text;
  std::error_code ec;
  if (std::filesystem::exists(target, ec)) {
    Result<std::string> read = read_file(target);
    if (read.is_err())
      return std::move(read).error();
    text = std::move(read).value();
  } else if (ec) {
    return Error("secrets: stat " + target.string() + ": " + ec.message());
  }
  Result<std::string> updated =
      replace_or_append_line(std::move(text), key, quoted.value());
  if (updated.is_err())
    return std::move(updated).error();
  Result<void> written = atomic_write(target, updated.value(), kPermPrivate);
  if (written.is_err())
    return std::move(written).error();
  return target;
}

Result<std::vector<std::string>> list_secret_keys(
    const Paths& paths, const std::string& service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("secrets: invalid service name: '" + service_name + "'");
  }
  const std::filesystem::path target =
      paths.secrets_dir / (service_name + ".env");
  std::vector<std::string> keys;
  std::error_code ec;
  if (!std::filesystem::exists(target, ec)) {
    if (ec)
      return Error("secrets: stat " + target.string() + ": " + ec.message());
    return keys;
  }
  Result<std::string> text = read_file(target);
  if (text.is_err())
    return std::move(text).error();
  const std::string& body = text.value();
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t newline = body.find('\n', start);
    const std::size_t end =
        newline == std::string::npos ? body.size() : newline;
    std::string key;
    const std::string_view line(body.data() + start, end - start);
    if (parse_line_key(line, &key) && valid_key(key))
      keys.push_back(std::move(key));
    if (newline == std::string::npos)
      break;
    start = newline + 1;
  }
  std::sort(keys.begin(), keys.end());
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (keys[i] == keys[i - 1]) {
      return Error("secrets: duplicate environment key: '" + keys[i] + "'");
    }
  }
  return keys;
}

}
