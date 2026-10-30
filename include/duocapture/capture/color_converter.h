#pragma once

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace duo {

#if defined(_WIN32)

// BGRA8 → NV12 的 GPU 转换。
//
// NVENC 只吃 NV12 / P010，而采集出来的是 BGRA。这一步必须在 GPU 上做：
// 回读到 CPU 再转换，1440p120 下光 PCIe 往返就要 4-6 ms，还会打满一个核。
//
// 实现说明：D3D11 不允许用一个 UAV 覆盖整张 NV12 纹理，必须按平面各建
// 一个视图——Y 平面用 R8_UNORM，色度平面用 R8G8_UNORM——因此转换分两次
// dispatch 完成。这是 D3D11 上 NV12 转换的标准做法。
//
// 色彩：BT.709、limited range（Y 16-235，Cb/Cr 16-240），与绝大多数游戏
// 和播放器的默认一致。HDR 需要改成 P010 + BT.2020 + PQ，属于后续扩展。
class ColorConverter {
 public:
  Status initialize(ID3D11Device* device, ID3D11DeviceContext* context);
  void shutdown();

  // 把 source（BGRA8）转换到内部 NV12 纹理。返回的纹理由转换器持有，
  // 下一次 convert() 会覆盖它——调用方必须在下次调用前消费完。
  Status convert(ID3D11Texture2D* source, ID3D11Texture2D** nv12_out);

  [[nodiscard]] u32 width() const noexcept { return width_; }
  [[nodiscard]] u32 height() const noexcept { return height_; }
  [[nodiscard]] ID3D11Texture2D* output_texture() const noexcept { return output_; }

 private:
  Status ensure_shader();
  Status ensure_output(u32 width, u32 height);
  Status ensure_input_srv(ID3D11Texture2D* source);
  void dispatch(ID3D11UnorderedAccessView* uav, u32 mode, u32 rows, u32 columns) const;

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;

  ID3D11ComputeShader* shader_ = nullptr;
  ID3D11Buffer* params_ = nullptr;
  ID3D11Texture2D* output_ = nullptr;
  ID3D11UnorderedAccessView* luma_uav_ = nullptr;
  ID3D11UnorderedAccessView* chroma_uav_ = nullptr;
  ID3D11ShaderResourceView* input_srv_ = nullptr;
  ID3D11Texture2D* input_texture_ = nullptr;
  u32 width_ = 0;
  u32 height_ = 0;
};

#else

// 非 Windows 平台没有 D3D11，接口保留为空实现，便于网络层单独构建与测试。
class ColorConverter {
 public:
  Status initialize(void*, void*) { return Status::NotSupported; }
  void shutdown() {}
  Status convert(void*, void**) { return Status::NotSupported; }
  [[nodiscard]] u32 width() const noexcept { return 0; }
  [[nodiscard]] u32 height() const noexcept { return 0; }
  [[nodiscard]] void* output_texture() const noexcept { return nullptr; }
};

#endif

}  // namespace duo
