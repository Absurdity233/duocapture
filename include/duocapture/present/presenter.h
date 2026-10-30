#pragma once

#include <memory>
#include <string>

#include "duocapture/common/status.h"
#include "duocapture/decode/decoder.h"

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace duo {

class IPresenter {
 public:
  virtual ~IPresenter() = default;

  virtual Status present(const DecodedFrame& frame) = 0;
  [[nodiscard]] virtual const char* backend_name() const = 0;
};

#if defined(_WIN32)

// Win32 窗口 + flip-model 交换链呈现器。
//
// 低延迟呈现的三个必要条件，缺一个都会白送 1-2 帧：
//   1. DXGI_SWAP_EFFECT_FLIP_DISCARD（老式 blt 交换链会多缓冲一帧）
//   2. SetMaximumFrameLatency(1)（默认是 2-3 帧）
//   3. Present(0, ALLOW_TEARING)（不等待 vsync，撕裂换低延迟）
//
// 语义：只显示最新帧。present() 立即返回，不排队。
//
// 解码器输出 NV12，交换链是 BGRA，中间走 ID3D11VideoProcessor——
// 它是 GPU 的固定功能单元，比 shader 路径便宜，且能正确处理
// BT.709 limited range 到 full range RGB 的映射。视频处理器与
// 枚举器在首次 present 时创建并缓存，之后每帧只做一次 Blt。
class Win32Presenter final : public IPresenter {
 public:
  Win32Presenter();
  ~Win32Presenter() override;

  Win32Presenter(const Win32Presenter&) = delete;
  Win32Presenter& operator=(const Win32Presenter&) = delete;

  Status initialize(const wchar_t* title, u32 width, u32 height);
  Status create_device();

  Status present(const DecodedFrame& frame) override;
  [[nodiscard]] const char* backend_name() const override { return "win32-flip-swapchain"; }

  // 消息泵：由主线程周期性调用，处理窗口消息与退出。
  [[nodiscard]] bool pump(bool block = false);

  [[nodiscard]] ID3D11Device* device() const noexcept;
  [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
  [[nodiscard]] bool should_close() const noexcept { return should_close_; }
  [[nodiscard]] u32 width() const noexcept { return width_; }
  [[nodiscard]] u32 height() const noexcept { return height_; }

 private:
  Status create_swapchain();
  Status ensure_video_processor(u32 input_width, u32 input_height);
  void cleanup();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  u32 width_ = 0;
  u32 height_ = 0;
  bool should_close_ = false;
};

#endif

}  // namespace duo
