#pragma once

#include <functional>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, std::function<void()> fn);
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

}  // namespace testfw

#define DUO_TEST(name)                                    \
  static void name();                                     \
  static ::testfw::Registrar duo_reg_##name(#name, name); \
  static void name()

#define DUO_CHECK(cond)                                                       \
  do {                                                                        \
    if (!(cond)) {                                                            \
      ::testfw::fail(__FILE__, __LINE__, "CHECK failed: " #cond);             \
    }                                                                         \
  } while (0)

#define DUO_CHECK_EQ(lhs, rhs)                                                \
  do {                                                                        \
    if (!((lhs) == (rhs))) {                                                  \
      ::testfw::fail(__FILE__, __LINE__, "CHECK_EQ failed: " #lhs " == " #rhs); \
    }                                                                         \
  } while (0)
