#pragma once

// src/berth/net/fetch.h — the entry point every module outside net/ uses.
// It is the only file that knows BOTH the HTTP client and the resolver, and
// wiring them here is what lets client.cpp and resolver.cpp depend on each
// other's absence instead of on each other.

#include <string>

#include "client.h"
#include "util/result.h"

namespace berth {

[[nodiscard]] Result<HttpReply> http_get(const std::string& url,
                                         const HttpGetOptions& options,
                                         const Paths& paths);

}  // namespace berth
