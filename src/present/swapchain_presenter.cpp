#include "duocapture/present/presenter.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "duocapture/common/log.h"

using Microsoft::WRL::ComPtr;

namespace duo {
namespace {

constexpr const wchar_t* kWindowClass = L"duocapture_presenter";

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        ::DestroyWindow(hwnd);
        return 0;
      }
      break;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

bool register_window_class() {
  static const bool registered = [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  return registered;
}

}  // namespace

struct Win32Presenter::Impl {
  HWND hwnd = nullptr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain1> swapchain;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<IDXGIFactory5> factory5;
  ComPtr<IDXGIAdapter> adapter;
  bool tearing_supported = false;

  // 视频处理器相关，按输入尺寸缓存。
  ComPtr<ID3D11VideoDevice> video_device;
  ComPtr<ID3D11VideoContext> video_context;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessor> processor;
  ComPtr<ID3D11VideoProcessorOutputView> output_view;
  ComPtr<ID3D11Texture2D> output_texture;
  u32 input_width = 0;
  u32 input_height = 0;
  u32 output_width = 0;
  u32 output_height = 0;
  TelemetryOverlay* overlay = nullptr;
  Win32Presenter::FrameOverlayFn frame_overlay = nullptr;
  void* frame_overlay_user = nullptr;
};

Win32Presenter::Win32Presenter() : impl_(std::make_unique<Impl>()) {}

Win32Presenter::~Win32Presenter() { cleanup(); }

Status Win32Presenter::create_device() {
  if (impl_->device != nullptr) {
    return Status::Ok;
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;

  HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &device, nullptr, &context);
  if (FAILED(hr)) {
    DUO_ERROR("D3D11 设备创建失败: 0x%08X", static_cast<unsigned>(hr));
    return Status::NotSupported;
  }

  // 视频处理器要求多线程保护开启，否则 VideoProcessorBlt 可能返回
  // DXGI_ERROR_INVALID_CALL。
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  impl_->device = device;
  impl_->context = context;
  return Status::Ok;
}

Status Win32Presenter::initialize(const wchar_t* title, u32 width, u32 height) {
  width_ = width;
  height_ = height;

  Status status = create_device();
  if (!ok(status)) {
    return status;
  }

  if (!register_window_class()) {
    return Status::NotSupported;
  }

  RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  ::AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

  impl_->hwnd = ::CreateWindowExW(0, kWindowClass, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                  nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (impl_->hwnd == nullptr) {
    return Status::IoError;
  }

  ::ShowWindow(impl_->hwnd, SW_SHOW);
  ::UpdateWindow(impl_->hwnd);

  return create_swapchain();
}

Status Win32Presenter::create_swapchain() {
  impl_->swapchain.Reset();

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(impl_->device.As(&dxgi_device))) {
    return Status::NotSupported;
  }
  if (FAILED(dxgi_device->GetAdapter(&impl_->adapter))) {
    return Status::NotSupported;
  }
  if (FAILED(impl_->adapter->GetParent(IID_PPV_ARGS(&impl_->factory)))) {
    return Status::NotSupported;
  }

  if (SUCCEEDED(impl_->factory.As(&impl_->factory5)) && impl_->factory5 != nullptr) {
    BOOL allow = FALSE;
    // 4 = DXGI_FEATURE_PRESENT_ALLOW_TEARING
    impl_->tearing_supported =
        SUCCEEDED(impl_->factory5->CheckFeatureSupport(static_cast<DXGI_FEATURE>(4), &allow,
                                                      sizeof(allow))) &&
        allow != FALSE;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  // 双缓冲足够：呈现端只显示最新帧，多缓冲只会增加延迟。
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (impl_->tearing_supported) {
    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  const HRESULT hr = impl_->factory->CreateSwapChainForHwnd(
      impl_->device.Get(), impl_->hwnd, &desc, nullptr, nullptr, &impl_->swapchain);
  if (FAILED(hr)) {
    DUO_ERROR("创建交换链失败: 0x%08X", static_cast<unsigned>(hr));
    return Status::NotSupported;
  }

  impl_->factory->MakeWindowAssociation(impl_->hwnd, DXGI_MWA_NO_ALT_ENTER);

  // 关键：把最大帧延迟压到 1，否则驱动会排队 2-3 帧。
  ComPtr<IDXGIDevice1> dxgi_device1;
  if (SUCCEEDED(impl_->device.As(&dxgi_device1))) {
    dxgi_device1->SetMaximumFrameLatency(1);
  }

  DUO_INFO("交换链就绪: %ux%u flip-discard, tearing=%d", width_, height_,
           impl_->tearing_supported ? 1 : 0);
  return Status::Ok;
}

Status Win32Presenter::ensure_video_processor(u32 input_width, u32 input_height) {
  if (impl_->processor != nullptr && impl_->input_width == input_width &&
      impl_->input_height == input_height && impl_->output_width == width_ &&
      impl_->output_height == height_) {
    return Status::Ok;
  }

  impl_->processor.Reset();
  impl_->enumerator.Reset();
  impl_->output_view.Reset();
  impl_->output_texture.Reset();

  if (impl_->video_device == nullptr) {
    if (FAILED(impl_->device.As(&impl_->video_device)) ||
        FAILED(impl_->context.As(&impl_->video_context))) {
      DUO_ERROR("当前设备不支持视频处理器");
      return Status::NotSupported;
    }
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = input_width;
  content.InputHeight = input_height;
  content.OutputWidth = width_;
  content.OutputHeight = height_;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  if (FAILED(impl_->video_device->CreateVideoProcessorEnumerator(&content, &impl_->enumerator))) {
    DUO_ERROR("创建视频处理器枚举器失败 %ux%u -> %ux%u", input_width, input_height, width_,
              height_);
    return Status::NotSupported;
  }
  if (FAILED(impl_->video_device->CreateVideoProcessor(impl_->enumerator.Get(), 0,
                                                       &impl_->processor))) {
    DUO_ERROR("创建视频处理器失败");
    return Status::NotSupported;
  }

  // 交换链后备缓冲不能直接作为视频处理器的输出目标，必须先 Blt 到一张
  // BGRA 纹理，再拷贝到后备缓冲。这是 D3D11 的限制。
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  if (FAILED(impl_->device->CreateTexture2D(&desc, nullptr, &impl_->output_texture))) {
    return Status::IoError;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
  view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  view_desc.Texture2D.MipSlice = 0;
  if (FAILED(impl_->video_device->CreateVideoProcessorOutputView(
          impl_->output_texture.Get(), impl_->enumerator.Get(), &view_desc,
          &impl_->output_view))) {
    DUO_ERROR("创建视频处理器输出视图失败");
    return Status::NotSupported;
  }

  impl_->input_width = input_width;
  impl_->input_height = input_height;
  impl_->output_width = width_;
  impl_->output_height = height_;
  return Status::Ok;
}

Status Win32Presenter::present(const DecodedFrame& frame) {
  if (impl_->swapchain == nullptr || frame.texture == nullptr) {
    return Status::NotInitialized;
  }

  auto* source = static_cast<ID3D11Texture2D*>(frame.texture);
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);

  Status status = ensure_video_processor(desc.Width, desc.Height);
  if (!ok(status)) {
    return status;
  }

  // 输入视图每帧创建：源纹理由解码器轮换，不能缓存。
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.FourCC = 0;
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.MipSlice = 0;
  input_desc.Texture2D.ArraySlice = 0;

  ComPtr<ID3D11VideoProcessorInputView> input_view;
  if (FAILED(impl_->video_device->CreateVideoProcessorInputView(
          source, impl_->enumerator.Get(), &input_desc, &input_view))) {
    return Status::IoError;
  }

  // BT.709 limited range -> full range RGB
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_space{};
  in_space.Usage = 0;
  in_space.RGB_Range = 0;
  in_space.YCbCr_Matrix = 1;  // BT.709
  in_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  impl_->video_context->VideoProcessorSetStreamColorSpace(impl_->processor.Get(), 0, &in_space);

  D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_space{};
  out_space.Usage = 0;
  out_space.RGB_Range = 1;
  out_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
  impl_->video_context->VideoProcessorSetOutputColorSpace(impl_->processor.Get(), &out_space);

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view.Get();

  const HRESULT blt_hr = impl_->video_context->VideoProcessorBlt(
      impl_->processor.Get(), impl_->output_view.Get(), 0, 1, &stream);
  if (FAILED(blt_hr)) {
    DUO_ERROR("VideoProcessorBlt 失败: 0x%08X", static_cast<unsigned>(blt_hr));
    return Status::IoError;
  }

  ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(impl_->swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
    return Status::IoError;
  }
  impl_->context->CopyResource(back_buffer.Get(), impl_->output_texture.Get());

  // 叠加层画在后备缓冲上，这样它不会被视频处理器覆盖。
  // 先画检测框（它是全画布半透明），再画遥测文字（左上角小块）。
  if (impl_->frame_overlay != nullptr) {
    impl_->frame_overlay(impl_->frame_overlay_user, back_buffer.Get(), width_, height_);
  }
  if (impl_->overlay != nullptr) {
    impl_->overlay->draw(back_buffer.Get(), width_, height_);
  }

  UINT flags = 0;
  if (impl_->tearing_supported) {
    flags |= DXGI_PRESENT_ALLOW_TEARING;
  }

  const HRESULT present_hr = impl_->swapchain->Present(0, flags);
  if (present_hr == DXGI_ERROR_DEVICE_REMOVED || present_hr == DXGI_ERROR_DEVICE_RESET) {
    DUO_ERROR("D3D 设备丢失，重建");
    impl_->processor.Reset();
    impl_->enumerator.Reset();
    impl_->output_view.Reset();
    impl_->output_texture.Reset();
    impl_->video_device.Reset();
    impl_->video_context.Reset();
    impl_->device.Reset();
    impl_->context.Reset();
    if (ok(create_device())) {
      create_swapchain();
    }
    return Status::Closed;
  }
  if (FAILED(present_hr)) {
    return Status::IoError;
  }
  return Status::Ok;
}

bool Win32Presenter::pump(bool block) {
  MSG msg{};
  if (block) {
    ::GetMessageW(&msg, nullptr, 0, 0);
    if (msg.message == WM_QUIT) {
      should_close_ = true;
      return false;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
    return true;
  }

  while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      should_close_ = true;
      return false;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  return true;
}

void Win32Presenter::cleanup() {
  impl_->output_view.Reset();
  impl_->output_texture.Reset();
  impl_->processor.Reset();
  impl_->enumerator.Reset();
  impl_->video_context.Reset();
  impl_->video_device.Reset();
  impl_->swapchain.Reset();
  impl_->factory5.Reset();
  impl_->factory.Reset();
  impl_->adapter.Reset();
  impl_->context.Reset();
  impl_->device.Reset();
  if (impl_->hwnd != nullptr) {
    ::DestroyWindow(impl_->hwnd);
    impl_->hwnd = nullptr;
  }
}

TelemetryOverlay* Win32Presenter::overlay() noexcept { return impl_->overlay; }

void Win32Presenter::set_overlay(TelemetryOverlay* overlay) noexcept {
  impl_->overlay = overlay;
}

void Win32Presenter::set_frame_overlay(FrameOverlayFn fn, void* user) noexcept {
  impl_->frame_overlay = fn;
  impl_->frame_overlay_user = user;
}

ID3D11Device* Win32Presenter::device() const noexcept { return impl_->device.Get(); }

ID3D11DeviceContext* Win32Presenter::context() const noexcept { return impl_->context.Get(); }

}  // namespace duo

#else

namespace duo {

}  // namespace duo

#endif
