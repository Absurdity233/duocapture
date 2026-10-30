// NVENC 编码器。
//
// 实现要点（M1 落地时按这个来，不要用编码器默认参数）：
//
//   preset       = NV_ENC_PRESET_P1_GUID
//   tuningInfo   = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY
//   frameIntervalP = 1              // 禁 B 帧
//   gopLength    = NVENC_INFINITE_GOPLENGTH + intraRefresh
//   rateControl  = CBR
//   vbvBufferSize = bitrate / fps   // ★ 1 帧缓冲，默认值会攒 2-3 帧
//   sliceMode    = 3 (每帧 N 片)
//   enableEncodeAsync = 1
//
// 需要 NVIDIA Video Codec SDK 头文件，构建时打开 -DDUO_ENABLE_NVENC=ON。
// 本文件在未启用时提供桩实现，保证接口与管线可以先跑通。

#include "duocapture/encode/encoder.h"

#if defined(DUO_ENABLE_NVENC)
#error "NVENC backend not implemented yet (M1). Remove DUO_ENABLE_NVENC to build the stub."
#endif

namespace duo {
namespace {

class NullEncoder final : public IEncoder {
 public:
  Status configure(const EncoderConfig&) override { return Status::NotSupported; }
  Status encode(const FrameView&, EncodedFrame&) override { return Status::NotSupported; }
  Status request_keyframe() override { return Status::NotSupported; }
  [[nodiscard]] const char* backend_name() const override { return "null"; }
};

}  // namespace

std::unique_ptr<IEncoder> create_encoder() { return nullptr; }

}  // namespace duo
