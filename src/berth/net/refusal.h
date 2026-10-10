#pragma once

// src/berth/net/refusal.h — the marker that separates "we refused to try"
// from "we tried and it failed". A refusal (host not allowlisted, plaintext
// http off loopback, a token bound for the wrong host) is a PERMANENT
// condition: the retry loop must not spend attempts on it, and a caller
// must not read it as a flaky network. Transport errors carry no marker and
// are retried.
//
// Marking lives on Error::root(), not on a typed field, so it survives
// every with_context() hop up the stack.

#include <string>
#include <string_view>
#include <utility>

#include "../../util/result.h"
#include "../../util/strutil.h"

namespace berth {

inline constexpr std::string_view kRefusalPrefix = "refused: ";

[[nodiscard]] inline bool is_refusal(const Error& e) {
  return starts_with(e.root(), kRefusalPrefix);
}

[[nodiscard]] inline Error refusal(std::string why) {
  return Error(std::string(kRefusalPrefix) + std::move(why));
}

}  // namespace berth
