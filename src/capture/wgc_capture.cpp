// Windows.Graphics.Capture 采集后端。
//
// 与 DDAPI 后端的分工：
//   * WGC 支持按窗口/显示器采集，HDR 与多显示器切换处理更完善，
//     并且不依赖"桌面复制会在独占全屏下失效"这一历史问题
//   * DDAPI 更轻量，静态桌面时能拿到脏矩形
//
// 这里用 free-threaded 帧池，回调直接落在调用线程之外，不需要 UI 线程消息泵。
//
// 已知约束：
//   * 帧池只有 2 个缓冲，必须尽快 close()，否则新帧会被丢弃
//   * 受保护内容（DRM / WDA）返回黑帧或直接失败，这里如实上报，不绕过

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

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"

namespace duo {
namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

// 采集源与 DDAPI 共用同一套显示器序号：先按 DXGI 输出拿到设备名
// （\\.\DISPLAY1），再映射到 HMONITOR。这样 --list-displays 的序号
// 在两个后端之间是一致的。
std::string display_device_name(u32 index) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return {};
  }

  std::string name;
  u32 global = 0;
  for (UINT a = 0; name.empty(); ++a) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    for (UINT o = 0;; ++o) {
      IDXGIOutput* output = nullptr;
      if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (global == index) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc))) {
          char narrow[64]{};
          ::WideCharToMultiByte(CP_ACP, 0, desc.DeviceName, -1, narrow, sizeof(narrow), nullptr,
                                nullptr);
          name = narrow;
        }
        output->Release();
        break;
      }
      output->Release();
      ++global;
    }
    adapter->Release();
  }
  factory->Release();
  return name;
}

class WgcCapture final : public ICaptureSource {
 public:
  ~WgcCapture() override { stop(); }

  Status start(u32 display_index) override {
    stop();

    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
      // 已经初始化过就继续
    }

    const std::string device_name = display_device_name(display_index);
    if (device_name.empty()) {
      return Status::InvalidArgument;
    }

    HMONITOR monitor = nullptr;

    struct MonitorSearch {
      const std::string* name;
      HMONITOR* result;
    } search{&device_name, &monitor};

    ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR handle, HDC, LPRECT, LPARAM param) -> BOOL {
          auto* ctx = reinterpret_cast<MonitorSearch*>(param);
          MONITORINFOEXA info{};
          info.cbSize = sizeof(info);
          if (::GetMonitorInfoA(handle, &info)) {
            if (*ctx->name == info.szDevice) {
              *ctx->result = handle;
              return FALSE;
            }
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    if (monitor == nullptr) {
      DUO_ERROR("未找到与 %s 对应的显示器", device_name.c_str());
      return Status::InvalidArgument;
    }

    // 采集所用的 D3D11 设备：优先 NVIDIA（编码器在同一块卡上，纹理不需要
    // 跨设备拷贝）。混合显卡机器上这一点决定了是否多一次 PCIe 往返。
    if (!create_device()) {
      return Status::NotSupported;
    }

    winrt::com_ptr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.as(IID_PPV_ARGS(dxgi_device.put())))) {
      return Status::NotSupported;
    }

    winrt::com_ptr<::IInspectable> inspectable;
    if (FAILED(::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()))) {
      return Status::NotSupported;
    }
    direct3d_device_ = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();

    const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                       IGraphicsCaptureItemInterop>();
    winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> raw_item;
    const HRESULT hr = interop->CreateForMonitor(
        monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        raw_item.put_void());
    if (FAILED(hr)) {
      DUO_ERROR("CreateForMonitor 失败: 0x%08X（受保护内容或不支持的系统版本）",
                static_cast<unsigned>(hr));
      return Status::NotSupported;
    }
    item_ = raw_item.as<wgc::GraphicsCaptureItem>();

    const auto size = item_.Size();
    width_ = static_cast<u32>(size.Width);
    height_ = static_cast<u32>(size.Height);

    frame_pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        direct3d_device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    session_ = frame_pool_.CreateCaptureSession(item_);
    session_.IsCursorCaptureEnabled(cursor_enabled_);

    // MinUpdateInterval 在 Windows 11 / 较新的 Win10 上可用。不支持时
    // WGC 会按显示器刷新率推送，对 120 Hz 场景反而更合适，忽略即可。
    try {
      session_.MinUpdateInterval(std::chrono::milliseconds(1));
    } catch (...) {
      DUO_INFO("当前系统不支持 MinUpdateInterval，按刷新率推送");
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = nullptr;
      stopping_ = false;
      last_status_ = Status::Ok;
    }

    frame_token_ = frame_pool_.FrameArrived(
        [this](const wgc::Direct3D11CaptureFramePool& sender, const winrt::Windows::Foundation::IInspectable&) {
          on_frame(sender);
        });

    session_.StartCapture();
    running_ = true;

    DUO_INFO("WGC 采集就绪: %ux%u, cursor=%d", width_, height_, cursor_enabled_ ? 1 : 0);
    return Status::Ok;
  }

  void stop() override {
    running_ = false;

    if (frame_pool_ != nullptr && static_cast<bool>(frame_token_)) {
      frame_pool_.FrameArrived(frame_token_);
      frame_token_ = {};
    }
    if (session_ != nullptr) {
      try {
        session_.Close();
      } catch (...) {
      }
      session_ = nullptr;
    }
    if (frame_pool_ != nullptr) {
      try {
        frame_pool_.Close();
      } catch (...) {
      }
      frame_pool_ = nullptr;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_ != nullptr) {
        pending_.Close();
        pending_ = nullptr;
      }
    }

    item_ = nullptr;
    direct3d_device_ = nullptr;
    context_ = nullptr;
    device_ = nullptr;
  }

  Status acquire(FrameView& out, u32 timeout_ms) override {
    if (!running_) {
      return Status::NotInitialized;
    }

    wgc::Direct3D11CaptureFrame frame{nullptr};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return pending_ != nullptr || !running_; })) {
        return Status::Timeout;
      }
      if (pending_ == nullptr) {
        return running_ ? Status::Timeout : Status::Closed;
      }
      frame = pending_;
      pending_ = nullptr;
    }

    // 把 WGC 的 surface 取成 ID3D11Texture2D。这里不拷贝，纹理直接交给
    // 后续的转换 shader。调用方 release() 时才 Close 帧。
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.put())))) {
      frame.Close();
      return Status::IoError;
    }

    texture->AddRef();  // 交给调用方，release() 里 Release
    out.texture = texture.get();
    out.width = width_;
    out.height = height_;
    out.capture_ts_ns = Clock::now_ns();
    out.cursor_visible = cursor_enabled_;

    // frame 对象交给 release() 关闭，借 texture 指针的伴生槽位保存。
    // 用单独一个 map 会更清晰，但每帧一次哈希查找在 120 fps 下不值得。
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hold_frame_ = frame;
    }
    return Status::Ok;
  }

  void release(FrameView& view) override {
    if (view.texture != nullptr) {
      static_cast<ID3D11Texture2D*>(view.texture)->Release();
      view.texture = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (hold_frame_ != nullptr) {
      hold_frame_.Close();
      hold_frame_ = nullptr;
    }
  }

  [[nodiscard]] const char* backend_name() const override { return "wgc"; }
  [[nodiscard]] void* d3d_device() const override { return const_cast<ID3D11Device*>(device_.get()); }
  [[nodiscard]] void* d3d_context() const override {
    return const_cast<ID3D11DeviceContext*>(context_.get());
  }

 private:
  void on_frame(const wgc::Direct3D11CaptureFramePool& sender) {
    wgc::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
    if (frame == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      frame.Close();
      return;
    }
    // 只保留最新帧：旧帧没人消费就直接丢掉，这正是呈现端要的语义。
    if (pending_ != nullptr) {
      pending_.Close();
    }
    pending_ = frame;
    cv_.notify_one();
  }

  bool create_device() {
    // 优先选 NVIDIA 适配器，让采集、转换、编码落在同一块 GPU 上。
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0;; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        adapter->Release();
        continue;
      }
      if (chosen == nullptr) {
        adapter = adapter;  // 第一个非软件适配器作为兜底
        chosen = adapter;
        continue;
      }
      // wcsstr 判断厂商名，NVIDIA 的 Description 里含 "NVIDIA"
      if (wcsstr(desc.Description, L"NVIDIA") != nullptr) {
        chosen->Release();
        chosen = adapter;
        break;
      }
      adapter->Release();
    }
    factory->Release();

    if (chosen == nullptr) {
      return false;
    }

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = ::D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                     D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    if (FAILED(hr)) {
      hr = ::D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, &levels[1], 1,
                               D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    }
    chosen->Release();
    return SUCCEEDED(hr);
  }

  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  wgdx::Direct3D11::IDirect3DDevice direct3d_device_{nullptr};
  wgc::GraphicsCaptureItem item_{nullptr};
  wgc::Direct3D11CaptureFramePool frame_pool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
  winrt::event_token frame_token_{};

  std::mutex mutex_;
  std::condition_variable cv_;
  wgc::Direct3D11CaptureFrame pending_{nullptr};
  wgc::Direct3D11CaptureFrame hold_frame_{nullptr};
  Status last_status_ = Status::Ok;

  u32 width_ = 0;
  u32 height_ = 0;
  bool cursor_enabled_ = true;
  bool running_ = false;
  bool stopping_ = false;
};

}  // namespace

std::unique_ptr<ICaptureSource> create_wgc_capture_source() {
  return std::make_unique<WgcCapture>();
}

}  // namespace duo

#else

namespace duo {

std::unique_ptr<ICaptureSource> create_wgc_capture_source() { return nullptr; }

}  // namespace duo

#endif
