#include "duocapture/common/clock.h"

#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace duo {

u64 Clock::now_ns() noexcept {
#if defined(_WIN32)
  // QueryPerformanceCounter 是 Windows 上精度最高且单调的时钟源。
  static const double ns_per_tick = [] {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    return 1e9 / static_cast<double>(freq.QuadPart);
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<u64>(static_cast<double>(counter.QuadPart) * ns_per_tick);
#else
  using namespace std::chrono;
  return static_cast<u64>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

}  // namespace duo
