#pragma once

// Wiring the client and the resolver here is what keeps them independent.

#include <string>

#include "client.h"
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<HttpReply> http_get(const std::string& url,
                                         const HttpGetOptions& options,
                                         const Paths& paths);

}
