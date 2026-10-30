#pragma once

#include <memory>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

// AAC-LC 解码器（Media Foundation）。
//
// 输入是带 ADTS 头的 AAC 帧（发送端直接吐这个格式），输出 16-bit
// 交错 PCM。AAC 解码 MFT 在 Windows 上始终可用，不依赖额外扩展。
class AacDecoder {
 public:
  AacDecoder();
  ~AacDecoder();

  AacDecoder(const AacDecoder&) = delete;
  AacDecoder& operator=(const AacDecoder&) = delete;

  Status open();
  void close();

  // 解一帧。out 是 16-bit 交错 PCM。解不出来时返回 Ok 但 out 为空
  // （解码器需要攒够数据），这是正常情况不是错误。
  Status decode(ByteSpan aac_adts_frame, std::vector<i16>& out);

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] u32 sample_rate() const noexcept;
  [[nodiscard]] u16 channels() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace duo
