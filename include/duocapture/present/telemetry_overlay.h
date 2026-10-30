#pragma once

#include <memory>
#include <string>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace duo {

struct OverlayLine {
  std::string label;
  std::string value;
};

// 遥测叠加层：把延迟、丢包、FEC 恢复率等信息画在画面上。
//
// 实现用 GDI + D2D 的混合方案里最省事的一种：离屏 DIB 画好文字，
// 再作为一张 D3D11 纹理叠在交换链上。不做字形缓存——这层只在
// 调试/监控时开，每秒刷一次，开销可以忽略。
class TelemetryOverlay {
 public:
  TelemetryOverlay();
  ~TelemetryOverlay();

  TelemetryOverlay(const TelemetryOverlay&) = delete;
  TelemetryOverlay& operator=(const TelemetryOverlay&) = delete;

#if defined(_WIN32)
  Status initialize(ID3D11Device* device, ID3D11DeviceContext* context);
  void shutdown();

  // 更新要显示的内容（每行一个 label/value）。
  void set_lines(const std::vector<OverlayLine>& lines);

  // 把叠加层画到 target 纹理上（通常是交换链后备缓冲）。
  Status draw(ID3D11Texture2D* target, u32 target_width, u32 target_height);
#endif

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

 private:
  bool enabled_ = true;

#if defined(_WIN32)
  struct Impl;
  std::unique_ptr<Impl> impl_;
#endif
};

}  // namespace duo
