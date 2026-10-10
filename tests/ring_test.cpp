// Tests for tui/ring.h (stage 6.1). The ring buffer exists because a log
// pane that grows forever is a bug waiting for a long deploy, so the tests
// are about the CEILING and the overwrite order, not about getting data in.

#include "tui/ring.h"

#include <string>
#include <vector>

#include "runner.h"

namespace {

std::vector<std::string> contents(const tui::RingBuffer<std::string>& ring) {
  return ring.snapshot();
}

}  // namespace

TEST(ring_starts_empty_and_reports_its_capacity) {
  const tui::RingBuffer<std::string> ring(4);
  CHECK(ring.empty());
  CHECK(ring.size() == 0);
  CHECK(ring.capacity() == 4);
  CHECK(ring.dropped() == 0);
  CHECK(contents(ring).empty());
}

TEST(ring_snapshot_is_oldest_first) {
  tui::RingBuffer<std::string> ring(4);
  ring.push_back("one");
  ring.push_back("two");
  ring.push_back("three");
  CHECK(ring.size() == 3);
  const std::vector<std::string> got = contents(ring);
  CHECK(got.size() == 3);
  CHECK(got[0] == "one");
  CHECK(got[1] == "two");
  CHECK(got[2] == "three");
}

TEST(ring_never_exceeds_its_capacity) {
  tui::RingBuffer<std::string> ring(3);
  for (int i = 0; i < 100; ++i) {
    ring.push_back("line " + std::to_string(i));
  }
  // The whole point: a bounded pane, whatever the producer does.
  CHECK(ring.size() == 3);
  CHECK(ring.capacity() == 3);
  CHECK(contents(ring).size() == 3);
}

TEST(ring_overwrites_the_oldest_and_counts_what_it_dropped) {
  tui::RingBuffer<std::string> ring(3);
  ring.push_back("a");
  ring.push_back("b");
  ring.push_back("c");
  CHECK(ring.dropped() == 0);
  ring.push_back("d");
  CHECK(ring.dropped() == 1);
  const std::vector<std::string> got = contents(ring);
  CHECK(got.size() == 3);
  CHECK(got[0] == "b");  // "a" was the oldest and is gone
  CHECK(got[1] == "c");
  CHECK(got[2] == "d");
  for (int i = 0; i < 10; ++i) {
    ring.push_back("x");
  }
  CHECK(ring.dropped() == 11);
  CHECK(ring.size() == 3);
}

TEST(ring_wraps_correctly_at_many_laps) {
  tui::RingBuffer<int> ring(5);
  for (int i = 0; i < 23; ++i) {
    ring.push_back(i);
  }
  const std::vector<int> got = ring.snapshot();
  CHECK(got.size() == 5);
  CHECK(got[0] == 18);
  CHECK(got[1] == 19);
  CHECK(got[2] == 20);
  CHECK(got[3] == 21);
  CHECK(got[4] == 22);
}

TEST(ring_clear_resets_the_dropped_counter) {
  tui::RingBuffer<std::string> ring(2);
  ring.push_back("a");
  ring.push_back("b");
  ring.push_back("c");
  CHECK(ring.dropped() == 1);
  ring.clear();
  CHECK(ring.empty());
  CHECK(ring.dropped() == 0);
  ring.push_back("fresh");
  const std::vector<std::string> got = contents(ring);
  CHECK(got.size() == 1);
  CHECK(got[0] == "fresh");
}

TEST(ring_at_reads_by_logical_index_without_copying) {
  // The deploy tail and the logs screen read the buffer straight from the
  // render thread, so at() has to agree with snapshot() AFTER a wrap — that is
  // the case where a naive index would read the wrong line.
  tui::RingBuffer<std::string> ring(3);
  ring.push_back("a");
  ring.push_back("b");
  ring.push_back("c");
  ring.push_back("d");
  CHECK(ring.dropped() == 1);
  CHECK(ring.at(0) == "b");
  CHECK(ring.at(1) == "c");
  CHECK(ring.at(2) == "d");
  const std::vector<std::string> got = contents(ring);
  for (std::size_t i = 0; i < got.size(); ++i) {
    CHECK(ring.at(i) == got[i]);
  }
}

TEST(ring_single_slot_keeps_only_the_newest) {
  tui::RingBuffer<int> ring(1);
  ring.push_back(1);
  ring.push_back(2);
  CHECK(ring.size() == 1);
  CHECK(ring.dropped() == 1);
  CHECK(ring.snapshot()[0] == 2);
}