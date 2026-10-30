#pragma once

#include <memory>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

#if defined(_WIN32)
#include <d3d11.h>
#else
// 非 Windows 平台上这些类型没有定义，但接口签名里要用到。
// 前向声明即可——真正带 D3D11 的实现只在 Windows 上编译。
struct ID3D11Device;
struct ID3D11DeviceContext;
#endif

namespace duo {

struct DecodedFrame {
  void* texture = nullptr;  // ID3D11Texture2D*（NV12），由解码器持有
  u32 width = 0;
  u32 height = 0;
  u64 capture_ts_ns = 0;
  bool key = false;
};

class IDecoder {
 public:
  virtual ~IDecoder() = default;

  // 绑定 D3D11 设备。需要与呈现器共用同一块设备的解码器实现它；
  // 不依赖 D3D11 的后端可以忽略。返回 NotSupported 表示该后端不支持。
  virtual Status bind_device(ID3D11Device* device, ID3D11DeviceContext* context) {
    (void)device;
    (void)context;
    return Status::NotSupported;
  }

  // first_access_unit：用于提取 SPS/PPS 构造 MF_MT_MPEG_SEQUENCE_HEADER。
  // 传空则跳过，某些解码器不需要它。
  virtual Status configure(Codec codec, ByteSpan first_access_unit) = 0;
  virtual Status decode(ByteSpan access_unit, u64 capture_ts_ns, DecodedFrame& out) = 0;
  virtual void release(DecodedFrame& frame) = 0;

  [[nodiscard]] virtual const char* backend_name() const = 0;
};

// 创建硬件解码器。当前没有可用后端时返回 nullptr。
std::unique_ptr<IDecoder> create_decoder();

#if defined(_WIN32)

// D3D11VA 解码器。
//
// 选它而不是 NVDEC 的理由：A 端不一定是 NVIDIA 显卡（推流机常见核显或
// A 卡），D3D11VA 在所有主流 GPU 上都能用，且解码输出直接落在 D3D11
// 纹理上，与交换链零拷贝对接。
//
// 实现要点：
//   * 用 ID3D11VideoDecoder + ID3D11VideoDecoderOutputView
//   * 输入是 Annex-B 的 H.264/HEVC 码流，需要自己切分 NAL 并构造
//     压缩数据（DXVA 的 PicParams / SliceControl）
//   * DPB 由驱动管理，这里只需按序提交
class D3d11vaDecoder final : public IDecoder {
 public:
  D3d11vaDecoder();
  ~D3d11vaDecoder() override;

  D3d11vaDecoder(const D3d11vaDecoder&) = delete;
  D3d11vaDecoder& operator=(const D3d11vaDecoder&) = delete;

  // device 必须与呈现器使用同一个。
  Status initialize(ID3D11Device* device, ID3D11DeviceContext* context);
  Status bind_device(ID3D11Device* device, ID3D11DeviceContext* context) override {
    return initialize(device, context);
  }

  Status configure(Codec codec, ByteSpan first_access_unit) override;
  Status decode(ByteSpan access_unit, u64 capture_ts_ns, DecodedFrame& out) override;
  void release(DecodedFrame& frame) override;

  [[nodiscard]] const char* backend_name() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif

}  // namespace duo
