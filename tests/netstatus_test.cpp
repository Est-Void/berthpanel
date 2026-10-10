// Tests for netstatus.h (stage 6.6): the per-session counters behind the NET
// banner. These are the values the RENDER THREAD reads every frame, so the
// interesting properties are (a) they are one consistent read, (b) a reset
// really zeroes the session, and (c) concurrent records from several threads
// neither race nor lose a count.

#include "core/netstatus.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/netlog.h"
#include "runner.h"
#include "test_env.h"

TEST(netstatus_starts_at_zero_for_the_session) {
  berth::netstatus_reset();
  const berth::NetSnapshot fresh = berth::netstatus_snapshot();
  CHECK(fresh.total == 0);
  CHECK(fresh.allowed == 0);
  CHECK(fresh.denied == 0);
  CHECK(fresh.last_host.empty());
}

TEST(netstatus_counts_allowed_and_refused_attempts_separately) {
  berth::netstatus_reset();
  berth::netstatus_record("api.github.com", true);
  berth::netstatus_record("github.com", true);
  berth::netstatus_record("evil.example.com", false);

  const berth::NetSnapshot snapshot = berth::netstatus_snapshot();
  CHECK(snapshot.total == 3);
  CHECK(snapshot.allowed == 2);
  CHECK(snapshot.denied == 1);
  // last_host is the most recent, refused or not: the panel shows what it last
  // tried, not what succeeded.
  CHECK(snapshot.last_host == "evil.example.com");
}

TEST(netstatus_reset_clears_every_field) {
  berth::netstatus_reset();
  berth::netstatus_record("proxy.golang.org", true);
  berth::netstatus_record("nope.example", false);
  CHECK(berth::netstatus_snapshot().total == 2);

  berth::netstatus_reset();
  const berth::NetSnapshot zeroed = berth::netstatus_snapshot();
  CHECK(zeroed.total == 0);
  CHECK(zeroed.allowed == 0);
  CHECK(zeroed.denied == 0);
  CHECK(zeroed.last_host.empty());
}

TEST(netstatus_increments_are_safe_from_several_threads) {
  berth::netstatus_reset();

  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([t]() {
      for (int i = 0; i < kPerThread; ++i) {
        berth::netstatus_record(t % 2 == 0 ? "github.com" : "cdn.jsdelivr.net",
                                i % 3 != 0);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  const berth::NetSnapshot snapshot = berth::netstatus_snapshot();
  const std::uint64_t attempts =
      static_cast<std::uint64_t>(kThreads * kPerThread);
  CHECK(snapshot.total == attempts);
  // Every third record in every thread is refused, so the denied count is
  // exact — not "about a third", which would be a test that cannot fail.
  const std::uint64_t denied_per_thread =
      static_cast<std::uint64_t>((kPerThread + 2) / 3);
  const std::uint64_t expected_denied =
      static_cast<std::uint64_t>(kThreads) * denied_per_thread;
  CHECK(snapshot.denied == expected_denied);
  CHECK(snapshot.allowed == attempts - expected_denied);
  CHECK(snapshot.total == snapshot.allowed + snapshot.denied);
}

TEST(netstatus_snapshots_are_never_a_torn_read) {
  // total == allowed + denied must hold even while another thread records:
  // the banner renders one snapshot, and a torn one would print a count that
  // never happened. This is the test that forces total+allowed into one atomic
  // word — three counters sampled separately fail here.
  berth::netstatus_reset();
  std::atomic<bool> stop{false};
  std::atomic<bool> inconsistent{false};
  std::thread writer([&stop]() {
    while (!stop.load()) {
      berth::netstatus_record("github.com", true);
      berth::netstatus_record("evil.example.com", false);
    }
  });
  for (int i = 0; i < 20000; ++i) {
    const berth::NetSnapshot snapshot = berth::netstatus_snapshot();
    if (snapshot.total != snapshot.allowed + snapshot.denied) {
      inconsistent.store(true);
      break;
    }
  }
  stop.store(true);
  writer.join();
  CHECK(!inconsistent.load());
  CHECK(berth::netstatus_snapshot().total > 0);
}

TEST(netstatus_counts_every_netlog_entry_including_refusals) {
  // The hook that makes the banner honest: netlog_append IS the count point,
  // so nothing can reach the network and stay invisible (§6.1).
  const TempDir tmp;
  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  paths.net_log = paths.state_dir / "net.log";

  berth::netstatus_reset();
  CHECK(berth::netlog_append(paths,
                             {
                                 .host = "github.com",
                                 .ip = "140.82.121.6",
                                 .port = 443,
                                 .purpose = "system-scan",
                                 .note = "",
                             })
            .is_ok());
  CHECK(berth::netlog_append(paths,
                             {
                                 .host = "not-allowlisted.example",
                                 .ip = "",
                                 .port = 443,
                                 .purpose = "https-get",
                                 .note = "refused by the allowlist",
                                 .allowed = false,
                             })
            .is_ok());

  const berth::NetSnapshot snapshot = berth::netstatus_snapshot();
  CHECK(snapshot.total == 2);
  CHECK(snapshot.denied == 1);
  CHECK(snapshot.last_host == "not-allowlisted.example");
}