#include "which.h"

#include <unistd.h>

#include <sys/stat.h>

#include <string>

#include "errno.h"
#include "strutil.h"

namespace berth {

Result<std::string> resolve_executable(const std::string& name) {
  if (name.empty())
    return Error("which: empty executable name");
  if (name.find('/') != std::string::npos) {
    return Error("which: name must not contain a path separator: " + name);
  }
  const std::optional<std::string> path = env_nonempty("PATH");
  if (!path.has_value())
    return Error("which: PATH is unset or empty");

  for (const std::string& dir : split(*path, ":")) {
    if (dir.empty())
      continue;
    std::string candidate = dir;
    candidate += '/';
    candidate += name;
    struct stat st{};
    if (::stat(candidate.c_str(), &st) != 0)
      continue;
    if (!S_ISREG(st.st_mode))
      continue;
    if (::access(candidate.c_str(), X_OK) != 0)
      continue;
    return candidate;
  }
  return Error("which: not found on PATH: " + name);
}

}
