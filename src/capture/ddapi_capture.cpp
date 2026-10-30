// DXGI Desktop Duplication 采集后端。
//
// 选它而不是 Windows.Graphics.Capture 的原因：
//   * 纯 COM，无 C++/WinRT 依赖，构建面小
//   * 直接给出脏矩形，静态桌面时可跳过编码
//   * 全屏独占游戏下最稳定，延迟最低
// 代价：无法按窗口采集，HDR 需要额外处理。这两点留给后续的 WGC 后端。
//
// 已知限制：display_index 是跨适配器的全局序号，与 list_displays() 顺序一致。

#include "duocapture/capture/capture_source.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "duocapture/common/clock.h"

using Microsoft::WRL::ComPtr;

namespace duo {
namespace {

std::string utf8_from_wide(const wchar_t* text) {
  if (text == nullptr) {
    return {};
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

ComPtr<IDXGIFactory1> make_factory() {
  ComPtr<IDXGIFactory1> factory;
  ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  return factory;
}

class DdapiCapture final : public ICaptureSource {
 public:
  ~DdapiCapture() override { stop(); }

  Status start(u32 display_index) override {
    stop();

    ComPtr<IDXGIFactory1> factory = make_factory();
    if (!factory) {
      return Status::NotSupported;
    }

    // 找到目标输出所在的适配器，设备必须建在同一块适配器上。
    ComPtr<IDXGIAdapter1> adapter;
    UINT local_index = 0;
    UINT global_index = 0;
    bool found = false;

    for (UINT a = 0; !found; ++a) {
      ComPtr<IDXGIAdapter1> candidate;
      if (factory->EnumAdapters1(a, &candidate) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> output;
        if (candidate->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (global_index == display_index) {
          adapter = candidate;
          local_index = o;
          found = true;
          break;
        }
        ++global_index;
      }
    }

    if (!found) {
      return Status::InvalidArgument;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                     D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
      hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, &levels[1], 1,
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
    }
    if (FAILED(hr)) {
      return Status::NotSupported;
    }

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(local_index, &output))) {
      return Status::InvalidArgument;
    }

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    width_ = static_cast<u32>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
    height_ = static_cast<u32>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    name_ = utf8_from_wide(desc.DeviceName);

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output1)))) {
      return Status::NotSupported;
    }
    if (FAILED(output1->DuplicateOutput(device_.Get(), &duplication_))) {
      return Status::NotSupported;
    }

    return Status::Ok;
  }

  void stop() override {
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    pending_release_ = false;
  }

  Status acquire(FrameView& out, u32 timeout_ms) override {
    if (!duplication_) {
      return Status::NotInitialized;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      return Status::Timeout;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
      return Status::Closed;
    }
    if (FAILED(hr)) {
      return Status::IoError;
    }

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) {
      duplication_->ReleaseFrame();
      return Status::IoError;
    }

    out.texture = texture.Detach();
    out.width = width_;
    out.height = height_;
    out.capture_ts_ns = Clock::now_ns();
    out.cursor_visible = info.PointerPosition.Visible != FALSE;
    pending_release_ = true;
    return Status::Ok;
  }

  void release(FrameView& view) override {
    if (view.texture != nullptr) {
      static_cast<ID3D11Texture2D*>(view.texture)->Release();
      view.texture = nullptr;
    }
    if (duplication_ && pending_release_) {
      duplication_->ReleaseFrame();
      pending_release_ = false;
    }
  }

  [[nodiscard]] const char* backend_name() const override { return "dxgi-desktop-duplication"; }

 private:
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  u32 width_ = 0;
  u32 height_ = 0;
  std::string name_;
  bool pending_release_ = false;
};

}  // namespace

std::vector<std::string> list_displays() {
  std::vector<std::string> out;
  ComPtr<IDXGIFactory1> factory = make_factory();
  if (!factory) {
    return out;
  }

  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_OUTPUT_DESC desc{};
      if (FAILED(output->GetDesc(&desc))) {
        continue;
      }
      const long w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
      const long h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
      char line[320];
      std::snprintf(line, sizeof(line), "%s  %ldx%ld  at (%ld,%ld)  %s",
                    utf8_from_wide(desc.DeviceName).c_str(), w, h,
                    static_cast<long>(desc.DesktopCoordinates.left),
                    static_cast<long>(desc.DesktopCoordinates.top),
                    desc.AttachedToDesktop ? "attached" : "detached");
      out.emplace_back(line);
    }
  }
  return out;
}

std::unique_ptr<ICaptureSource> create_capture_source() {
  return std::make_unique<DdapiCapture>();
}

}  // namespace duo

#else  // !_WIN32

#include <string>
#include <vector>

namespace duo {

std::vector<std::string> list_displays() { return {}; }

std::unique_ptr<ICaptureSource> create_capture_source() { return nullptr; }

}  // namespace duo

#endif  // _WIN32
