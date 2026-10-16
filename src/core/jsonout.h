#pragma once

// Every --json document carries a schema version and renders deterministically;
// array order is the caller's job (each builder sorts by name).

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace berth {

// Bump on removal or meaning change, never for an added optional field.
inline constexpr int kJsonSchema = 1;

[[nodiscard]] inline nlohmann::json json_document(
    const std::string_view command) {
  nlohmann::json doc;
  doc["schema"] = kJsonSchema;
  doc["command"] = std::string(command);
  return doc;
}

// Exactly what goes to stdout — no log line ever shares the stream (§5).
[[nodiscard]] inline std::string json_text(const nlohmann::json& doc) {
  return doc.dump(2) + "\n";
}

}
