#pragma once

// src/util/which.h — resolve an executable to an ABSOLUTE path by
// searching PATH. subprocess.run() demands an absolute argv[0] (§4: argv
// arrays only, no PATH search inside exec), and spawning "systemctl" with
// a bare name would silently depend on the caller's environment. Resolving
// here keeps that decision visible and testable (toolcheck reuses this).

#include <string>

#include "env.h"
#include "result.h"

namespace berth {

// First PATH hit that exists and is executable (regular file or symlink to
// one). Empty or unset PATH is an error, not "search nowhere".
[[nodiscard]] Result<std::string> resolve_executable(const std::string& name);

}  // namespace berth
