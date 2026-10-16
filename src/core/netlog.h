#pragma once

// Every attempt — allowed, refused, failed — lands here; this module owns no
// sockets.

#include <cstdint>
#include <string_view>

#include "core/allowlist.h"
#include "core/paths.h"
#include "util/result.h"

namespace berth {

struct NetlogEntry {
  std::string_view host;
  std::string_view ip;  // attempted address; may be empty
  std::uint16_t port = 0;
  std::string_view purpose;
  std::string_view note;
  bool allowed = true;  // false marks a refused attempt, which is still logged
};

// Thread-safe: one mutex, one O_APPEND write per entry, into the 0600 net.log.
[[nodiscard]] Result<void> netlog_append(const Paths& paths,
                                         const NetlogEntry& entry);

}
