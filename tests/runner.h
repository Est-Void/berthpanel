#pragma once

// Hand-rolled test runner (dev.md §9): a TEST macro and a global registry,
// deliberately dependency-free, deliberately single-threaded. Environment-
// touching tests serialize on util/env_test_lock() once that exists (stage
// 2.1); this runner owns nothing outside tests/.

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int run_all() {
  std::size_t failed = 0;
  for (const Case& c : registry()) {
    std::printf("[ RUN  ] %s\n", c.name);
    try {
      c.fn();
      std::printf("[  OK  ] %s\n", c.name);
    } catch (const std::exception& e) {
      ++failed;
      std::printf("[ FAIL ] %s: %s\n", c.name, e.what());
    }
  }
  std::printf("%zu/%zu passed\n", registry().size() - failed,
              registry().size());
  return failed == 0 ? 0 : 1;
}

}
#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      throw std::runtime_error(std::string(__FILE__) + ":" + \
                               std::to_string(__LINE__) +    \
                               ": CHECK failed: " #cond);    \
    }                                                        \
  } while (false)

#define TEST(name)                                          \
  static void test_fn_##name();                             \
  namespace {                                               \
  [[maybe_unused]] const bool test_reg_##name = [] {        \
    ::test::registry().push_back({#name, &test_fn_##name}); \
    return true;                                            \
  }();                                                      \
  }                                                         \
  static void test_fn_##name()
