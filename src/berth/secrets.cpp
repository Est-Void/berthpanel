// src/berth/secrets.cpp — see secrets.h for the contract.

#include "secrets.h"

#include <algorithm>
#include <string>

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

// Values are always quoted: an unquoted value would stop at the first
// space or '#', and systemd would silently store a truncated secret.
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

}  // namespace

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

}  // namespace berth
