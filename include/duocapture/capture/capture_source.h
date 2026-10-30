#pragma once

#include <memory>
#include <string>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

// 一帧采集结果。texture 指向 GPU 纹理（ID3D11Texture2D*），**不拷贝**。
// 调用方必须在 release() 之前完成消费，否则纹理会被回收到帧池。
struct FrameView {
  void* texture = nullptr;
  u32 width = 0;
  u32 height = 0;
  u64 capture_ts_ns = 0;
  bool cursor_visible = false;
};

class ICaptureSource {
 public:
  virtual ~ICaptureSource() = default;

  virtual Status start(u32 display_index) = 0;
  virtual void stop() = 0;

  // 取一帧。超时返回 Status::Timeout；采集会话失效返回 Status::Closed
  // （调用方应 stop() 后重新 start()）。
  virtual Status acquire(FrameView& out, u32 timeout_ms) = 0;
  virtual void release(FrameView& view) = 0;

  [[nodiscard]] virtual const char* backend_name() const = 0;

  // 采集所用 D3D11 设备（ID3D11Device*），用于把后续的转换/编码放到同一
  // 块设备上，避免纹理跨设备拷贝。无 D3D11 后端的平台返回 nullptr。
  [[nodiscard]] virtual void* d3d_device() const { return nullptr; }
  [[nodiscard]] virtual void* d3d_context() const { return nullptr; }
};

// 枚举可采集的显示器，顺序与 start() 的 display_index 一致。
std::vector<std::string> list_displays();

// 创建采集源。当前平台无可用后端时返回 nullptr。
// prefer_wgc 为 true 时优先用 Windows.Graphics.Capture，失败则回退 DDAPI。
std::unique_ptr<ICaptureSource> create_capture_source(bool prefer_wgc = true);

// 直接创建指定后端，不做回退。任一后端不可用时返回 nullptr。
std::unique_ptr<ICaptureSource> create_wgc_capture_source();
std::unique_ptr<ICaptureSource> create_ddapi_capture_source();

}  // namespace duo
