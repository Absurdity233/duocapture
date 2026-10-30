#pragma once

#include <memory>
#include <string>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/plugin/plugin_api.h"

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace duo {

// 检测框叠加层：把插件上报的框画到呈现表面上。
//
// 为什么由宿主画、而不是插件直接往解码纹理上写：分析线程和呈现线程
// 共用同一张解码纹理（receiver 把同一个 DecodedFrame 同时交给两边），
// 插件直接写会和 VideoProcessorBlt 抢同一块显存，画面会闪。
//
// 实现方式与 TelemetryOverlay 一致：离屏 BGRA 画布 → 纹理 → 在
// 后备缓冲上 alpha 混合。框的数量是几十个量级，CPU 光栅化足够，
// 不需要几何着色器。
class BoxOverlay {
 public:
  BoxOverlay();
  ~BoxOverlay();

  BoxOverlay(const BoxOverlay&) = delete;
  BoxOverlay& operator=(const BoxOverlay&) = delete;

#if defined(_WIN32)
  Status initialize(ID3D11Device* device, ID3D11DeviceContext* context);
  void shutdown();

  // 把 boxes 画到 target（通常是交换链后备缓冲）。
  //
  // box 的坐标以 source_width/height 为基准，target 是 target_width/height；
  // 两者长宽比不同时按拉伸映射（与视频处理器的行为一致，它默认 Stretch）。
  Status draw(ID3D11Texture2D* target, u32 target_width, u32 target_height,
              const DuoOverlayBox* boxes, int count, u32 source_width, u32 source_height);
#endif

  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }

 private:
  bool enabled_ = true;

#if defined(_WIN32)
  struct Impl;
  std::unique_ptr<Impl> impl_;
#endif
};

}  // namespace duo
