// Tests for util/result.h (stage 1.1): context chaining across boundaries,
// outermost-first ctx() ordering, value/error movement, Result<void>.
// The abort paths (value() on err, error() on ok) are invariant violations
// by definition and are not exercisable in-process.

#include "util/result.h"

#include <string>
#include <utility>

#include "runner.h"

using berth::Error;
using berth::Result;
using berth::with_context;

namespace {

Result<int> root_fails() {
  return Error("enoent");
}

// Cross-type propagation: an inner Result<int>'s error becomes the outer
// Result<std::string>'s error, adding one context layer on the way.
Result<std::string> middle_fails() {
  Result<int> r = root_fails();
  if (r.is_err())
    return with_context(std::move(r).error(), "reading state.json");
  return std::to_string(r.value());
}

Result<std::string> top_fails() {
  Result<std::string> r = middle_fails();
  if (r.is_err())
    return with_context(std::move(r), "registering service");
  return r.value();
}

}  // namespace

TEST(result_ok_holds_value) {
  const Result<int> r = 42;
  CHECK(r.is_ok());
  CHECK(!r.is_err());
  CHECK(r.value() == 42);
}

TEST(result_context_chain_outermost_first) {
  const Result<std::string> r = top_fails();
  CHECK(r.is_err());
  CHECK(r.error().ctx() == "registering service: reading state.json: enoent");
  CHECK(r.error().root() == "enoent");
}

TEST(result_error_moves_out) {
  Result<std::string> r = Error("root cause");
  Error e = std::move(r).error();
  e.add_context("outer layer");
  CHECK(e.ctx() == "outer layer: root cause");
}

TEST(result_move_out_value) {
  Result<std::string> r = std::string("payload");
  std::string v = std::move(r).value();
  CHECK(v == "payload");
}

TEST(result_void_ok) {
  const Result<void> r;
  CHECK(r.is_ok());
  CHECK(!r.is_err());
}

TEST(result_void_err_chains) {
  Result<void> r = Error("bad json");
  r = with_context(std::move(r), "loading registry");
  CHECK(r.is_err());
  CHECK(r.error().ctx() == "loading registry: bad json");
}

TEST(with_context_is_noop_on_ok) {
  Result<int> r = 7;
  const Result<int> wrapped = with_context(std::move(r), "unused context");
  CHECK(wrapped.is_ok());
  CHECK(wrapped.value() == 7);
}
