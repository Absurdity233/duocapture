#pragma once

#include <memory>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

namespace duo {

struct DecodedFrame {
  void* texture = nullptr;  // ID3D11Texture2D*，NV12
  u32 width = 0;
  u32 height = 0;
  u64 capture_ts_ns = 0;
  bool key = false;
};

class IDecoder {
 public:
  virtual ~IDecoder() = default;

  virtual Status configure(Codec codec) = 0;
  virtual Status decode(ByteSpan access_unit, u64 capture_ts_ns, DecodedFrame& out) = 0;
  virtual void release(DecodedFrame& frame) = 0;

  [[nodiscard]] virtual const char* backend_name() const = 0;
};

// 创建硬件解码器。当前没有可用后端时返回 nullptr。
std::unique_ptr<IDecoder> create_decoder();

}  // namespace duo
