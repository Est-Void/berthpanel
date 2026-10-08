// Tests for tui/ops.h (stage 6.7). This is the whole quit path's foundation:
// a flag nothing polls is the §11 bug, so the properties that matter are that
// cancel_all() REACHES every live operation, that a finished one is forgotten
// (so a later cancel cannot flip a flag nobody reads), and that the shutdown
// wait is BOUNDED and honest about failing.

#include "tui/ops.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "runner.h"

using tui::Op;
using tui::OpRegistry;

TEST(ops_register_and_forget_an_operation) {
  OpRegistry ops;
  CHECK(ops.empty());
  CHECK(ops.size() == 0);

  const std::shared_ptr<Op> first = ops.begin("deploy berthpanel-memos");
  CHECK(first != nullptr);
  CHECK(first->label == "deploy berthpanel-memos");
  CHECK(!first->cancel.load());
  CHECK(ops.size() == 1);
  CHECK(!ops.empty());

  CHECK(ops.end(first));
  CHECK(ops.empty());
  // Ending it twice is not a crash and not a silent lie either: it says no.
  CHECK(!ops.end(first));
}

TEST(ops_cancel_all_reaches_every_live_operation) {
  // The invariant: q and Ctrl-C reach the workers. One flag per operation, so
  // the deploy can be cancelled without touching a log refresh.
  OpRegistry ops;
  const std::shared_ptr<Op> deploy = ops.begin("deploy berthpanel-memos");
  const std::shared_ptr<Op> scan = ops.begin("system scan");

  CHECK(ops.cancel_all() == 2);
  CHECK(deploy->cancel.load());
  CHECK(scan->cancel.load());
  // Idempotent, because the quit path and the signal watcher both call it.
  CHECK(ops.cancel_all() == 2);
}

TEST(ops_cancel_all_leaves_a_finished_operation_alone) {
  // A cancelled-but-forgotten flag is harmless; a flag flipped on an op whose
  // worker already returned would be a lie in the shutdown report.
  OpRegistry ops;
  const std::shared_ptr<Op> done = ops.begin("log tail");
  CHECK(ops.end(done));
  CHECK(ops.cancel_all() == 0);
  CHECK(!done->cancel.load());
}

TEST(ops_cancel_all_on_an_empty_registry_is_a_no_op) {
  OpRegistry ops;
  CHECK(ops.cancel_all() == 0);
  CHECK(ops.empty());
}

TEST(ops_wait_returns_at_once_when_nothing_is_running) {
  OpRegistry ops;
  const auto started = std::chrono::steady_clock::now();
  CHECK(ops.wait_until_empty(std::chrono::milliseconds(2000)));
  // It must not have slept the whole budget: "nothing to wait for" has to be
  // fast or every idle quit costs two seconds.
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(500));
}

TEST(ops_wait_notices_an_operation_that_stops) {
  OpRegistry ops;
  const std::shared_ptr<Op> worker = ops.begin("system scan");
  std::thread stopper([&ops, worker]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ops.cancel_all();  // the quit, from another thread
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    static_cast<void>(ops.end(worker));
  });
  CHECK(ops.wait_until_empty(std::chrono::milliseconds(3000)));
  stopper.join();
  CHECK(ops.empty());
}

TEST(ops_wait_is_bounded_and_says_so_when_a_worker_ignores_the_cancel) {
  // The case that must NOT hang the quit: a worker wedged in a syscall. The
  // wait gives up and the caller reports a detached operation loudly (§11).
  OpRegistry ops;
  const std::shared_ptr<Op> wedged = ops.begin("deploy berthpanel-memos");
  const auto started = std::chrono::steady_clock::now();
  CHECK(!ops.wait_until_empty(std::chrono::milliseconds(250)));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= std::chrono::milliseconds(200));
  CHECK(elapsed < std::chrono::seconds(2));
  CHECK(ops.cancel_all() == 1);  // it was still asked to stop
  CHECK(wedged->cancel.load());
  CHECK(ops.end(wedged));
}

TEST(ops_survive_concurrent_begin_and_end) {
  // The registry is touched by every worker on its way in and out; the list
  // must not be corrupted or leak entries under that.
  OpRegistry ops;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&ops, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::shared_ptr<Op> op = ops.begin("op " + std::to_string(t));
        if (i % 3 == 0) {
          static_cast<void>(ops.cancel_all());
        }
        static_cast<void>(ops.end(op));
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  CHECK(ops.empty());
  CHECK(ops.size() == 0);
}

TEST(
    wait_for_flag_notices_a_flag_that_arrives_and_gives_up_on_one_that_does_not) {
  // The deploy tail's handshake uses this, and so does anything else that has
  // to wait for another thread without ever waiting forever.
  std::atomic<bool> flag{false};
  const auto started = std::chrono::steady_clock::now();
  CHECK(!tui::wait_for_flag(flag, std::chrono::milliseconds(150)));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= std::chrono::milliseconds(100));
  CHECK(elapsed < std::chrono::seconds(2));

  std::thread setter([&flag]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    flag.store(true);
  });
  CHECK(tui::wait_for_flag(flag, std::chrono::milliseconds(3000)));
  setter.join();
  // Already set: returns at once.
  const auto immediate = std::chrono::steady_clock::now();
  CHECK(tui::wait_for_flag(flag, std::chrono::milliseconds(2000)));
  CHECK(std::chrono::steady_clock::now() - immediate <
        std::chrono::milliseconds(200));
}

TEST(ops_a_cancelled_flag_is_visible_to_a_polling_worker) {
  // The worker half of the contract: a loop that polls its flag stops. This is
  // what makes "the engine unwinds" true instead of aspirational.
  OpRegistry ops;
  const std::shared_ptr<Op> worker = ops.begin("deploy berthpanel-memos");
  std::atomic<int> iterations{0};
  std::atomic<bool> saw_cancel{false};
  std::thread poller([&worker, &iterations, &saw_cancel]() {
    while (!worker->cancel.load(std::memory_order_relaxed)) {
      iterations.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (iterations.load() > 200) {
        break;  // the test must not hang if the flag never arrives
      }
    }
    saw_cancel.store(worker->cancel.load());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(ops.cancel_all() == 1);
  poller.join();
  CHECK(saw_cancel.load());
  CHECK(iterations.load() > 0);  // it really was polling, not a no-op
  static_cast<void>(ops.end(worker));
}