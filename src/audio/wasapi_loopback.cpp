#include "duocapture/audio/wasapi_loopback.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "duocapture/common/log.h"

using Microsoft::WRL::ComPtr;

namespace duo {
namespace {

template <typename T>
void safe_release(T*& ptr) {
  if (ptr != nullptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

}  // namespace

struct WasapiLoopback::Impl {
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;

  AudioFormat format{};
  bool running = false;
  bool com_initialized = false;

  std::vector<float> pending;

  void release_all() {
    capture.Reset();
    client.Reset();
    device.Reset();
    enumerator.Reset();
    running = false;
  }
};

WasapiLoopback::WasapiLoopback() : impl_(std::make_unique<Impl>()) {}

WasapiLoopback::~WasapiLoopback() { stop(); }

Status WasapiLoopback::start(int device_index) {
  if (impl_->running) {
    return Status::InvalidArgument;
  }

  // 采集线程自己初始化 COM（MTA）。重复初始化返回 RPC_E_CHANGED_MODE
  // 也无所谓，说明已经有 MTA 了。
  const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  impl_->com_initialized = SUCCEEDED(com_hr);

  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&impl_->enumerator));
  if (FAILED(hr)) {
    DUO_ERROR("创建 MMDeviceEnumerator 失败: 0x%08X", static_cast<unsigned>(hr));
    return Status::NotSupported;
  }

  if (device_index < 0) {
    hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
  } else {
    // 按序号枚举渲染设备
    ComPtr<IMMDeviceCollection> collection;
    hr = impl_->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr)) {
      UINT count = 0;
      collection->GetCount(&count);
      if (static_cast<UINT>(device_index) < count) {
        hr = collection->Item(static_cast<UINT>(device_index), &impl_->device);
      } else {
        hr = E_INVALIDARG;
      }
    }
  }
  if (FAILED(hr) || impl_->device == nullptr) {
    DUO_ERROR("获取播放设备失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &impl_->client);
  if (FAILED(hr)) {
    DUO_ERROR("激活 IAudioClient 失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  WAVEFORMATEX* mix_format = nullptr;
  hr = impl_->client->GetMixFormat(&mix_format);
  if (FAILED(hr) || mix_format == nullptr) {
    DUO_ERROR("GetMixFormat 失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  impl_->format.sample_rate = mix_format->nSamplesPerSec;
  impl_->format.channels = mix_format->nChannels;
  impl_->format.bits_per_sample = mix_format->wBitsPerSample;

  // 环回采集必须用共享模式 + AUDCLNT_STREAMFLAGS_LOOPBACK。
  // 缓冲给 200ms：太短容易溢出丢数据，太长会引入延迟。
  const REFERENCE_TIME buffer_duration = 2'000'000;  // 200ms
  hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 buffer_duration, 0, mix_format, nullptr);
  ::CoTaskMemFree(mix_format);
  if (FAILED(hr)) {
    DUO_ERROR("IAudioClient::Initialize 失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->client->GetService(IID_PPV_ARGS(&impl_->capture));
  if (FAILED(hr)) {
    DUO_ERROR("GetService(IAudioCaptureClient) 失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->client->Start();
  if (FAILED(hr)) {
    DUO_ERROR("IAudioClient::Start 失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  impl_->running = true;
  DUO_INFO("WASAPI 环回就绪: %u Hz, %u 声道", impl_->format.sample_rate,
           impl_->format.channels);
  return Status::Ok;
}

void WasapiLoopback::stop() {
  if (impl_->client != nullptr) {
    impl_->client->Stop();
  }
  impl_->release_all();
  if (impl_->com_initialized) {
    ::CoUninitialize();
    impl_->com_initialized = false;
  }
}

bool WasapiLoopback::running() const noexcept { return impl_->running; }

const AudioFormat& WasapiLoopback::format() const noexcept { return impl_->format; }

bool WasapiLoopback::read(std::vector<float>& out, u32 timeout_ms) {
  out.clear();
  if (!impl_->running || impl_->capture == nullptr) {
    return false;
  }

  // 环回设备在没有声音播放时不会产生数据，这里等一下再返回 false。
  if (timeout_ms > 0) {
    ::Sleep(timeout_ms);
  }

  UINT32 packet_frames = 0;
  if (FAILED(impl_->capture->GetNextPacketSize(&packet_frames)) || packet_frames == 0) {
    return false;
  }

  bool got = false;
  while (packet_frames > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(impl_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
      break;
    }

    const std::size_t samples = static_cast<std::size_t>(frames) * impl_->format.channels;
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
      // 静音包：补零，保持时间轴连续，否则音画会漂移。
      out.insert(out.end(), samples, 0.0f);
    } else if (data != nullptr) {
      const auto* f = reinterpret_cast<const float*>(data);
      out.insert(out.end(), f, f + samples);
    }
    impl_->capture->ReleaseBuffer(frames);
    got = true;

    if (FAILED(impl_->capture->GetNextPacketSize(&packet_frames))) {
      break;
    }
  }
  return got;
}

void WasapiLoopback::to_s16(const std::vector<float>& in, std::vector<i16>& out) {
  out.resize(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const float clamped = std::clamp(in[i], -1.0f, 1.0f);
    out[i] = static_cast<i16>(clamped * 32767.0f);
  }
}

}  // namespace duo

#else  // !_WIN32

namespace duo {

struct WasapiLoopback::Impl {};

WasapiLoopback::WasapiLoopback() : impl_(std::make_unique<Impl>()) {}
WasapiLoopback::~WasapiLoopback() = default;
Status WasapiLoopback::start(int) { return Status::NotSupported; }
void WasapiLoopback::stop() {}
bool WasapiLoopback::running() const noexcept { return false; }
const AudioFormat& WasapiLoopback::format() const noexcept {
  static const AudioFormat empty{};
  return empty;
}
bool WasapiLoopback::read(std::vector<float>&, u32) { return false; }
void WasapiLoopback::to_s16(const std::vector<float>&, std::vector<i16>&) {}

}  // namespace duo

#endif
