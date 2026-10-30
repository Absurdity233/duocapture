#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

struct AudioFormat {
  u32 sample_rate = 48000;
  u16 channels = 2;
  u16 bits_per_sample = 32;  // WASAPI 共享模式固定 32-bit float
};

// WASAPI 环回采集（采集"系统正在播放的声音"）。
//
// 用于 B 端把游戏/应用的声音一并送出。环回设备是渲染端点的
// 镜像，所以不需要虚拟声卡，也不用改默认播放设备。
//
// 采集出来的是 32-bit float 交错采样，Opus 编码前要转成 16-bit。
class WasapiLoopback {
 public:
  WasapiLoopback();
  ~WasapiLoopback();

  WasapiLoopback(const WasapiLoopback&) = delete;
  WasapiLoopback& operator=(const WasapiLoopback&) = delete;

  // device_index = -1 表示默认播放设备。
  Status start(int device_index = -1);
  void stop();

  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] const AudioFormat& format() const noexcept;

  // 读一段数据。返回 false 表示暂时没有新数据（正常，不是错误）。
  // out 里是 float 交错采样，长度按帧对齐。
  bool read(std::vector<float>& out, u32 timeout_ms);

  // 转成 16-bit 交错 PCM（Opus 输入格式）。
  static void to_s16(const std::vector<float>& in, std::vector<i16>& out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace duo
