#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include "testfw.h"

namespace testfw {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* name, std::function<void()> fn) {
  registry().push_back(TestCase{name, std::move(fn)});
}

void fail(const char* file, int line, const std::string& message) {
  throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + "  " + message);
}

}  // namespace testfw

int main() {
  int failed = 0;
  for (const auto& test : testfw::registry()) {
    try {
      test.fn();
      std::printf("[PASS] %s\n", test.name.c_str());
    } catch (const std::exception& error) {
      ++failed;
      std::printf("[FAIL] %s: %s\n", test.name.c_str(), error.what());
    }
  }

  std::printf("\n%zu 个测试，%d 个失败\n", testfw::registry().size(), failed);
  return failed == 0 ? 0 : 1;
}
