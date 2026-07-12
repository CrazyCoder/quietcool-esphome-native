#pragma once
// Minimal zero-dep C++17 test framework.
// Usage:
//   TEST("description") { REQUIRE(x == y); }
// Compile + run: g++ -std=c++17 test_*.cpp -o test && ./test

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tu {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(std::string n, std::function<void()> f) {
    registry().push_back({std::move(n), std::move(f)});
  }
};

inline int run_all() {
  int passed = 0, failed = 0;
  for (auto& tc : registry()) {
    try {
      tc.fn();
      std::cout << "  PASS  " << tc.name << "\n";
      ++passed;
    } catch (const std::exception& e) {
      std::cout << "  FAIL  " << tc.name << "\n        " << e.what() << "\n";
      ++failed;
    }
  }
  std::cout << "\n" << passed << " passed, " << failed << " failed.\n";
  return failed ? 1 : 0;
}

}  // namespace tu

#define TU_CAT_(a, b) a##b
#define TU_CAT(a, b) TU_CAT_(a, b)
#define TU_UNIQUE(prefix) TU_CAT(prefix, __LINE__)

#define TEST(name)                                                            \
  static void TU_UNIQUE(_tu_fn_)();                                           \
  static ::tu::Registrar TU_UNIQUE(_tu_reg_)(name, &TU_UNIQUE(_tu_fn_));      \
  static void TU_UNIQUE(_tu_fn_)()

#define REQUIRE(expr)                                                         \
  do {                                                                        \
    if (!(expr)) {                                                            \
      std::ostringstream _ss;                                                 \
      _ss << "REQUIRE failed: " #expr " at " << __FILE__ << ":" << __LINE__;  \
      throw std::runtime_error(_ss.str());                                    \
    }                                                                         \
  } while (0)

// Variadic in the expected slot so brace-init lists with commas work, e.g.
//   REQUIRE_EQ(foo(), RelayState{false, false, false});
#define REQUIRE_EQ(actual, ...)                                               \
  do {                                                                        \
    const auto _a = (actual);                                                 \
    const auto _e = __VA_ARGS__;                                              \
    if (!(_a == _e)) {                                                        \
      std::ostringstream _ss;                                                 \
      _ss << "REQUIRE_EQ failed at " << __FILE__ << ":" << __LINE__           \
          << "\n          actual:   " << _a << "\n          expected: " << _e;\
      throw std::runtime_error(_ss.str());                                    \
    }                                                                         \
  } while (0)
