#include "core/flags.h"

#include <mutex>
#include <optional>
#include <string>

#include "runner.h"
#include "test_env.h"
#include "util/env.h"

using berth::env_flag;

namespace {
const char* kFlag = "BERTHPANEL_TEST_FLAG";

}
TEST(flags_unset_is_nullopt) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv f(kFlag);
  f.unset();
  const auto r = env_flag(kFlag);
  CHECK(r.is_ok());
  CHECK(r.value() == std::nullopt);
}

TEST(flags_empty_is_nullopt) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv f(kFlag);
  f.set("");  // §11: empty is unset, not false
  const auto r = env_flag(kFlag);
  CHECK(r.is_ok());
  CHECK(r.value() == std::nullopt);
}

TEST(flags_truthy_values) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv f(kFlag);
  for (const char* v : {"1", "true", "TRUE", "Yes", " on ", "on"}) {
    f.set(v);
    const auto r = env_flag(kFlag);
    CHECK(r.is_ok());
    CHECK(r.value() == std::optional<bool>(true));
  }
}

TEST(flags_falsy_values) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv f(kFlag);
  for (const char* v : {"0", "false", "False", "No", " off ", "off"}) {
    f.set(v);
    const auto r = env_flag(kFlag);
    CHECK(r.is_ok());
    CHECK(r.value() == std::optional<bool>(false));
  }
}

TEST(flags_garbage_is_loud_error) {
  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv f(kFlag);
  for (const char* v : {"maybe", "2", "on,", "enabled"}) {
    f.set(v);
    const auto r = env_flag(kFlag);
    CHECK(r.is_err());
    CHECK(r.error().ctx().find(kFlag) != std::string::npos);
  }
}
