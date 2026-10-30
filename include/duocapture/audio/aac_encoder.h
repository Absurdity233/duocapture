#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

// AAC-LC 编码器（Media Foundation）。
//
// 为什么不是 Opus：这台机器上没有 Opus 编码器（系统只有 MP3 / AAC /
// FLAC / WMA 等 MFT），引入 libopus 要额外拖一个第三方库。AAC-LC 在
// 128-192kbps 下听感与 Opus 同级，延迟也只差几毫秒，对直播足够。
//
// 输入：48kHz 16-bit 交错 PCM。输出：带 ADTS 头的 AAC 帧（可直接
// 写进 MKV 的 A_AAC 轨道，或喂给 RTMP）。
class AacEncoder {
 public:
  AacEncoder();
  ~AacEncoder();

  AacEncoder(const AacEncoder&) = delete;
  AacEncoder& operator=(const AacEncoder&) = delete;

  Status open(u32 sample_rate, u16 channels, u32 bitrate_bps);
  void close();

  // 喂一段 PCM（16-bit 交错）。编码器会按 1024 采样一帧切分，
  // 返回这次能吐出来的所有 AAC 帧（可能为空）。
  Status encode(ByteSpan pcm_s16, std::vector<Bytes>& out);

  // 冲刷编码器里残留的数据。
  Status flush(std::vector<Bytes>& out);

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] u32 sample_rate() const noexcept;
  [[nodiscard]] u16 channels() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace duo
