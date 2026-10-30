#include "duocapture/audio/wasapi_render.h"

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

struct WasapiRender::Impl {
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;

  WAVEFORMATEX* mix_format = nullptr;
  bool running = false;
  bool com_initialized = false;

  UINT32 buffer_frames = 0;

  // 设备是 float32 时用这个中转
  std::vector<float> scratch;

  void release_all() {
    if (mix_format != nullptr) {
      ::CoTaskMemFree(mix_format);
      mix_format = nullptr;
    }
    render.Reset();
    client.Reset();
    device.Reset();
    enumerator.Reset();
    running = false;
  }
};

WasapiRender::WasapiRender() : impl_(std::make_unique<Impl>()) {}

WasapiRender::~WasapiRender() { stop(); }

Status WasapiRender::start(u32 sample_rate, u16 channels) {
  if (impl_->running) {
    return Status::InvalidArgument;
  }

  const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  impl_->com_initialized = SUCCEEDED(com_hr);

  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&impl_->enumerator));
  if (FAILED(hr)) {
    return Status::NotSupported;
  }

  hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
  if (FAILED(hr)) {
    DUO_ERROR("没有可用的播放设备: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &impl_->client);
  if (FAILED(hr)) {
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->client->GetMixFormat(&impl_->mix_format);
  if (FAILED(hr) || impl_->mix_format == nullptr) {
    impl_->release_all();
    return Status::NotSupported;
  }

  // 20ms 缓冲：音画同步与欠载之间的折中。
  const REFERENCE_TIME duration = 200'000;  // 20ms
  hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, duration, 0, impl_->mix_format,
                                 nullptr);
  if (FAILED(hr)) {
    DUO_ERROR("音频播放初始化失败: 0x%08X", static_cast<unsigned>(hr));
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->client->GetBufferSize(&impl_->buffer_frames);
  if (FAILED(hr)) {
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->client->GetService(IID_PPV_ARGS(&impl_->render));
  if (FAILED(hr)) {
    impl_->release_all();
    return Status::NotSupported;
  }

  hr = impl_->client->Start();
  if (FAILED(hr)) {
    impl_->release_all();
    return Status::NotSupported;
  }

  impl_->running = true;
  DUO_INFO("音频播放就绪: 设备 %u Hz / %u 声道，源 %u Hz / %u 声道",
           impl_->mix_format->nSamplesPerSec, impl_->mix_format->nChannels, sample_rate, channels);
  return Status::Ok;
}

void WasapiRender::stop() {
  if (impl_->client != nullptr) {
    impl_->client->Stop();
  }
  impl_->release_all();
  if (impl_->com_initialized) {
    ::CoUninitialize();
    impl_->com_initialized = false;
  }
}

bool WasapiRender::running() const noexcept { return impl_->running; }

Status WasapiRender::write(ByteSpan pcm_s16) {
  if (!impl_->running || impl_->render == nullptr || impl_->mix_format == nullptr) {
    return Status::NotInitialized;
  }
  if (pcm_s16.empty()) {
    return Status::Ok;
  }

  UINT32 padding = 0;
  if (FAILED(impl_->client->GetCurrentPadding(&padding))) {
    return Status::IoError;
  }

  // 缓冲满了直接丢——音频丢几十毫秒听不出来，阻塞才是灾难。
  const UINT32 free_frames = impl_->buffer_frames - padding;
  if (free_frames == 0) {
    return Status::Ok;
  }

  const u16 src_channels = 2;
  const std::size_t src_frame_bytes = src_channels * sizeof(i16);
  const std::size_t avail_frames = pcm_s16.size() / src_frame_bytes;
  const UINT32 frames = static_cast<UINT32>(
      std::min<std::size_t>(static_cast<std::size_t>(free_frames), avail_frames));
  if (frames == 0) {
    return Status::Ok;
  }

  BYTE* data = nullptr;
  if (FAILED(impl_->render->GetBuffer(frames, &data))) {
    return Status::IoError;
  }

  // 设备格式通常是 32-bit float 立体声。这里做 16-bit → float 转换。
  // 设备声道数不为 2 时只处理前两个声道（简单起见，够用）。
  const bool need_convert = impl_->mix_format->wBitsPerSample == 32;
  if (need_convert) {
    impl_->scratch.resize(static_cast<std::size_t>(frames) * src_channels);
    const auto* src = reinterpret_cast<const i16*>(pcm_s16.data());
    for (std::size_t i = 0; i < impl_->scratch.size(); ++i) {
      impl_->scratch[i] = static_cast<float>(src[i]) / 32768.0f;
    }
    std::memcpy(data, impl_->scratch.data(),
                impl_->scratch.size() * sizeof(float));
  } else {
    std::memcpy(data, pcm_s16.data(), static_cast<std::size_t>(frames) * src_frame_bytes);
  }

  impl_->render->ReleaseBuffer(frames, 0);
  return Status::Ok;
}

}  // namespace duo

#else

namespace duo {

struct WasapiRender::Impl {};

WasapiRender::WasapiRender() : impl_(std::make_unique<Impl>()) {}
WasapiRender::~WasapiRender() = default;
Status WasapiRender::start(u32, u16) { return Status::NotSupported; }
void WasapiRender::stop() {}
bool WasapiRender::running() const noexcept { return false; }
Status WasapiRender::write(ByteSpan) { return Status::NotSupported; }

}  // namespace duo

#endif
