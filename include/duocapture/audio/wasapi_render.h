#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

// WASAPI 音频播放（A 端出声）。
//
// 用共享模式 + 事件驱动，缓冲压到 20ms：太大会让音画不同步，
// 太小容易欠载爆音。共享模式不需要独占设备，不会打断用户其他声音。
class WasapiRender {
 public:
  WasapiRender();
  ~WasapiRender();

  WasapiRender(const WasapiRender&) = delete;
  WasapiRender& operator=(const WasapiRender&) = delete;

  Status start(u32 sample_rate, u16 channels);
  void stop();

  [[nodiscard]] bool running() const noexcept;

  // 写入 PCM（16-bit 交错）。内部会转成设备要的格式（通常是 float）。
  // 缓冲满时直接丢弃，不阻塞——音频丢一点听不出来，卡住解码线程
  // 才是致命的。
  Status write(ByteSpan pcm_s16);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace duo
