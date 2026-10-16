#pragma once

#include <string>

#include "env.h"
#include "result.h"

namespace berth {

// Returns an ABSOLUTE path: run() rejects a bare argv[0], and a PATH search
// inside exec would hide the resolution from the caller. Empty or unset PATH
// is an error, not "search nowhere".
[[nodiscard]] Result<std::string> resolve_executable(const std::string& name);

}
