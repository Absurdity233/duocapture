#pragma once

#include <memory>

#include "duocapture/capture/capture_source.h"
#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

enum class Codec { H264, Hevc, Av1 };

struct EncoderConfig {
  Codec codec = Codec::Hevc;
  u32 width = 0;
  u32 height = 0;
  u32 fps = 120;
  u32 bitrate_bps = 80'000'000;

  // 低延迟关键参数，默认值就是推荐值，不要用编码器的默认值。
  bool intra_refresh = true;  // 用 intra refresh 代替周期性 IDR，避免整帧卡顿
  u32 gop_frames = 0;         // 0 表示无限 GOP（配合 intra refresh）
  u32 slice_count = 4;        // 每帧切片数，降低单包丢失的影响面
};

struct EncodedFrame {
  Bytes data;
  bool key = false;
  u64 capture_ts_ns = 0;
};

class IEncoder {
 public:
  virtual ~IEncoder() = default;

  virtual Status configure(const EncoderConfig& config) = 0;
  virtual Status encode(const FrameView& frame, EncodedFrame& out) = 0;
  virtual Status request_keyframe() = 0;

  [[nodiscard]] virtual const char* backend_name() const = 0;
};

// 创建硬件编码器。当前没有可用后端时返回 nullptr。
std::unique_ptr<IEncoder> create_encoder();

}  // namespace duo
