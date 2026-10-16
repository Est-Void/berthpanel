#pragma once

// A refusal is permanent: never retried, never read as a flaky network. The
// marker lives on Error::root() so it survives every with_context() hop.

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

}
