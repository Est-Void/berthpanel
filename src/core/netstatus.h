#pragma once

// Per-session counters behind the NET banner, read from the render path on
// every frame: nothing added here may block or do I/O.

#include <cstdint>
#include <string>
#include <string_view>

namespace berth {

struct NetSnapshot {
  std::uint64_t total = 0;
  std::uint64_t allowed = 0;
  std::uint64_t denied = 0;  // refused by the allowlist, still logged
  std::string last_host;
};

// One attempt, allowed or refused. netlog_append calls this BEFORE writing the
// log line, so the count may be one higher than the log, never lower.
void netstatus_record(std::string_view host, bool allowed);

// One consistent read: total and allowed from the same word, denied derived.
[[nodiscard]] NetSnapshot netstatus_snapshot();

// Per session, not per file: a counter that did not reset would claim requests
// this panel never made.
void netstatus_reset();

}
