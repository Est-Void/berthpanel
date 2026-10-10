#pragma once

// src/core/netstatus.h — the per-session request counters behind the NET
// banner in the panel's header (dev.md §5, §6.1).
//
// net.log is the durable, auditable record and the only thing §6.1 actually
// requires. These counters are the same facts in the shape the RENDER THREAD
// can read on every frame: three words, no file, no lock, no allocation on the
// hot path. Reading the log file per frame is exactly the "no blocking call on
// the UI thread" bug (§5), so the counts have to be readable as data.
//
// Lock-free where it can be, and consistent where it matters. total/allowed
// live in ONE atomic word and denied is derived from them, because a reader
// that samples two counters separately can observe a moment that never
// happened — and a banner printing a count that never was is worse than no
// banner at all. last_host is an atomic shared_ptr, because
// std::atomic<std::string> does not exist: reading it is a pointer load plus a
// refcount bump — no mutex of ours, no I/O, no allocation — while the ONE
// allocation per record happens on the network path, never on a render. Do NOT
// add anything to this header that needs to block: it is read from the render
// path.

#include <cstdint>
#include <string>
#include <string_view>

namespace berth {

struct NetSnapshot {
  std::uint64_t total = 0;  // every recorded outbound attempt
  std::uint64_t allowed = 0;
  std::uint64_t denied = 0;  // refused by the allowlist — still logged (§6.1)
  std::string last_host;     // empty until something has been recorded
};

// One attempt, allowed or refused. Called by netlog_append BEFORE the log line
// is written, so a request the process never got to log is still counted: the
// count may be one higher than the log, never one lower.
void netstatus_record(std::string_view host, bool allowed);

// The counts as ONE read: total and allowed from the same atomic word, denied
// derived, last_host best-effort from its own pointer.
[[nodiscard]] NetSnapshot netstatus_snapshot();

// Per SESSION, not per file: the panel starts at zero and counts what THIS
// process asked for. net.log survives across runs; a counter that did not reset
// would claim requests this panel never made.
void netstatus_reset();

}  // namespace berth