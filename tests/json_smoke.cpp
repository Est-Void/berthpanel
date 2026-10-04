#include <nlohmann/json.hpp>

#include "runner.h"

using json = nlohmann::json;

TEST(json_parse_smoke) {
  const char* text = R"({"service":"memos","port":8234,"tags":["a","b"]})";
  const json j = json::parse(text);
  CHECK(j.at("service") == "memos");
  CHECK(j.at("port") == 8234);
  CHECK(j.at("tags").size() == 2);
}

TEST(json_roundtrip_smoke) {
  const json j = json::parse(R"({"service":"memos","port":8234})");
  const json parsed = json::parse(j.dump());
  CHECK(parsed == j);
}

TEST(json_parse_error_throws) {
  bool threw = false;
  try {
    [[maybe_unused]] const json j = json::parse("{not json");
  } catch (const json::parse_error&) {
    threw = true;
  }
  CHECK(threw);
}
