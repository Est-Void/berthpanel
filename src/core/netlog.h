#pragma once

// src/core/netlog.h — the net.log WRITER (dev.md §6.1). The allowlist policy
// it enforces lives in allowlist.h, which this header re-exports so callers
// need one include. Every attempt — allowed, refused, failed — lands here,
// before and after the request, and is counted in netstatus.h for the panel's
// NET banner. Owns no sockets.

#include <cstdint>
#include <string_view>

#include "core/allowlist.h"
#include "core/paths.h"
#include "util/result.h"

namespace berth {

struct NetlogEntry {
  std::string_view host;
  std::string_view ip;  // connected/attempted address; may be empty
  std::uint16_t port = 0;
  std::string_view purpose;  // "https-get", "doh", "egress-probe", ...
  std::string_view note;     // outcome: status code, error, stop reason
  bool allowed = true;       // false marks a refused attempt (still logged!)
};

// Appends one ISO-8601 UTC line to net.log (0600). Thread-safe: a mutex
// plus a single O_APPEND write per entry.
[[nodiscard]] Result<void> netlog_append(const Paths& paths,
                                         const NetlogEntry& entry);

}  // namespace berth
