#include "names.h"

#include <string>

#include "util/ascii.h"
#include "util/strutil.h"

namespace berth {

bool validate_unit_name(const std::string_view name) {
  if (name.size() > kMaxUnitNameLen)
    return false;
  if (!starts_with(name, kUnitPrefix))
    return false;
  const std::string_view stem = name.substr(kUnitPrefix.size());
  return !stem.empty() && all_chars(stem, is_unit_stem_char);
}

std::string force_unit_prefix(const std::string_view name) {
  if (starts_with(name, kUnitPrefix))
    return std::string(name);
  return std::string(kUnitPrefix) + std::string(name);
}

Result<std::string> build_unit_name(const std::string_view service_name) {
  if (!validate_unit_name(service_name)) {
    return Error("names: invalid service name: '" + std::string(service_name) +
                 "'");
  }
  const std::string stem =
      std::string(kBuildUnitInfix) + std::string(service_name);
  // Validated separately: a legal service name can still overflow the cap; an
  // unnameable build is one cleanup could never stop (§11).
  if (!validate_unit_name(stem)) {
    return Error("names: service name '" + std::string(service_name) +
                 "' does not leave room for a valid build unit name '" + stem +
                 "'");
  }
  return stem;
}

}
