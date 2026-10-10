#pragma once

// src/core/jsonout.h — the schema marker and the one stdout rendering shared
// by every `--json` output (plan 5.3). A wrapper consumer needs two things
// this header makes non-negotiable: a version it can branch on and bytes that
// do not move between runs. nlohmann's default object is a std::map, so key
// order is already deterministic; array order is the caller's job (each
// builder sorts by name).

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace berth {

// Bump only when a field is removed or its meaning changes; adding an
// optional field does NOT need a bump.
inline constexpr int kJsonSchema = 1;

// The envelope every document starts from: the schema version and the
// command that produced it.
[[nodiscard]] inline nlohmann::json json_document(
    const std::string_view command) {
  nlohmann::json doc;
  doc["schema"] = kJsonSchema;
  doc["command"] = std::string(command);
  return doc;
}

// Exactly what goes to stdout: pretty JSON and a trailing newline, nothing
// else (no log line ever shares the stream; dev.md §5).
[[nodiscard]] inline std::string json_text(const nlohmann::json& doc) {
  return doc.dump(2) + "\n";
}

}  // namespace berth
