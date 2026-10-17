#pragma once

// Secret rotation over a deployed service (stage 10.4): the write is
// secrets.cpp's 0600 atomic path and the restart is injected, so the engine is
// testable without a live systemd. No redeploy, no journal entry, no unit
// rewrite — only the environment file and the running process change.

#include <functional>
#include <string>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

using ServiceRestart =
    std::function<Result<void>(const std::string& service_name)>;

[[nodiscard]] Result<void> rotate_secret(const Paths& paths,
                                         const std::string& service_name,
                                         const std::string& key,
                                         const std::string& value,
                                         const ServiceRestart& restart);

}
