// Tests for port.h (stage 4.2): picks an unprivileged loopback port and
// honors the avoid list of already-assigned ports.

#include "berth/port.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "runner.h"

using berth::pick_free_port;

TEST(port_pick_returns_unprivileged_port) {
  const auto r = pick_free_port();
  CHECK(r.is_ok());
  CHECK(r.value() >= 1024);
}

TEST(port_pick_avoids_already_assigned_ports) {
  std::vector<std::uint16_t> assigned;
  for (int i = 0; i < 20; ++i) {
    const auto r = pick_free_port(assigned);
    CHECK(r.is_ok());
    const std::uint16_t port = r.value();
    CHECK(port >= 1024);
    const bool duplicate =
        std::find(assigned.begin(), assigned.end(), port) != assigned.end();
    CHECK(!duplicate);
    assigned.push_back(port);
  }
}
