#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/decode/decoder.h"

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace duo {

#if defined(_WIN32)

// NVDEC 硬件解码器（直接调 CUVID，不经过 Media Foundation）。
//
// 为什么不用 MF：这台机器上 MFTEnumEx(HARDWARE) 对 H.264/HEVC 都返回 0，
// NVIDIA 驱动没有把 NVDEC 注册成 MF 的 Video Decoder MFT。但硬件本身可用
// （cuvidGetDecoderCaps 对 H264/HEVC/VP9/AV1 全部 supported=1，8192x8192）。
//
// 结构：
//   cuvidCreateVideoParser 解析 Annex-B → pfnDecodePicture 回调 cuvidDecodePicture
//   → pfnDisplayPicture 回调里 cuvidMapVideoFrame64 拿到 CUdeviceptr
//   → 通过 D3D11 互操作注册的纹理拷贝出来（或直接在 CUDA 里拷）
//
// 线程约束：CUVID 与 CUDA 上下文都绑定线程，所有调用必须在同一线程上。
class NvdecDecoder final : public IDecoder {
 public:
  NvdecDecoder();
  ~NvdecDecoder() override;

  NvdecDecoder(const NvdecDecoder&) = delete;
  NvdecDecoder& operator=(const NvdecDecoder&) = delete;

  // device 必须与呈现器同一个，才能把解码结果零拷贝到交换链。
  Status initialize(ID3D11Device* device, ID3D11DeviceContext* context);
  Status bind_device(ID3D11Device* device, ID3D11DeviceContext* context) override {
    return initialize(device, context);
  }

  Status configure(Codec codec, ByteSpan first_access_unit) override;
  Status decode(ByteSpan access_unit, u64 capture_ts_ns, DecodedFrame& out) override;
  void release(DecodedFrame& frame) override;

  [[nodiscard]] const char* backend_name() const override { return "nvdec-cuvid"; }
  [[nodiscard]] bool hardware_accelerated() const noexcept;

  // 是否尝试 D3D11 零拷贝。默认关闭：NV12 的 UV 平面拿不到第二个
  // CUDA array slice，这条路径目前只对 Y 平面有效。关闭时走
  // CUDA 显存 → CPU → UpdateSubresource，实测够用。
  void set_zero_copy(bool enabled) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 这台机器上 NVDEC 是否可用（只查能力，不建解码器）。
bool nvdec_available() noexcept;

#endif

}  // namespace duo
