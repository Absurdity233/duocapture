#pragma once

#include "duocapture/common/types.h"

namespace duo {

// 单调时钟（纳秒）。管线内所有时间戳——采集、发送、到达、呈现——都以此为准，
// 避免跨机器使用墙上时钟导致的时钟漂移问题。
class Clock {
 public:
  static u64 now_ns() noexcept;
  static double to_ms(u64 ns) noexcept { return static_cast<double>(ns) / 1e6; }
  static u64 from_ms(double ms) noexcept { return static_cast<u64>(ms * 1e6); }
};

}  // namespace duo
