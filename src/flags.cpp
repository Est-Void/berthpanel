#include "flags.h"

#include <cctype>
#include <string>

#include "util/env.h"
#include "util/strutil.h"

namespace berth {

Result<std::optional<bool>> env_flag(const char* name) {
  const std::optional<std::string> raw = env_nonempty(name);
  if (!raw.has_value()) return std::optional<bool>{};

  std::string value = std::string(trim(*raw));
  for (char& c : value) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }

  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return std::optional<bool>{true};
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return std::optional<bool>{false};
  }
  return Error("flags: " + std::string(name) + ": unrecognized value '" +
               *raw + "'");
}

}  // namespace berth
