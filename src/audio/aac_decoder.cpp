#include "duocapture/audio/aac_decoder.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>

#include "duocapture/common/log.h"

using Microsoft::WRL::ComPtr;

namespace duo {

struct AacDecoder::Impl {
  ComPtr<IMFTransform> transform;
  bool opened = false;
  u32 sample_rate = 0;
  u16 channels = 0;
  i64 next_timestamp = 0;
  std::mutex mutex;
};

AacDecoder::AacDecoder() : impl_(std::make_unique<Impl>()) {}

AacDecoder::~AacDecoder() { close(); }

Status AacDecoder::open() {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (impl_->opened) {
    return Status::Ok;
  }

  static const bool mf_ok = [] {
    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    return SUCCEEDED(hr) || hr == MF_E_ALREADY_INITIALIZED;
  }();
  if (!mf_ok) {
    return Status::NotSupported;
  }

  MFT_REGISTER_TYPE_INFO in_info{};
  in_info.guidMajorType = MFMediaType_Audio;
  in_info.guidSubtype = MFAudioFormat_AAC;

  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  const HRESULT hr = ::MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                                 nullptr, &acts, &count);
  if (FAILED(hr) || count == 0) {
    DUO_ERROR("没有可用的 AAC 解码器 MFT");
    return Status::NotSupported;
  }

  ComPtr<IMFTransform> chosen;
  for (UINT32 i = 0; i < count; ++i) {
    ComPtr<IMFTransform> candidate;
    if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
      chosen = candidate;
      break;
    }
  }
  for (UINT32 i = 0; i < count; ++i) {
    acts[i]->Release();
  }
  ::CoTaskMemFree(acts);

  if (chosen == nullptr) {
    return Status::NotSupported;
  }

  // 输入类型：直接从 MFT 给出的候选里挑 ADTS（payloadType=1）那个。
  //
  // 手工构造会失败：MF 的 AAC 解码器要求 MF_MT_USER_DATA 是 12 字节的
  // HEXAAC 结构（4 字节头 + 8 字节扩展 + ASC），自己拼很容易漏字段，
  // 结果是 MF_E_INVALIDTYPE(0xC00D36B4)。候选类型里什么都填好了。
  ComPtr<IMFMediaType> in_type;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> candidate;
    if (chosen->GetInputAvailableType(0, i, &candidate) != S_OK) {
      break;
    }
    GUID subtype{};
    if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFAudioFormat_AAC) {
      continue;
    }
    UINT32 payload_type = 0;
    if (SUCCEEDED(candidate->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload_type)) &&
        payload_type == 1) {
      in_type = candidate;
      break;
    }
    if (in_type == nullptr) {
      in_type = candidate;  // 兜底：任意 AAC 类型
    }
  }

  if (in_type == nullptr) {
    DUO_ERROR("AAC 解码器没有可用的输入类型");
    return Status::NotSupported;
  }
  // 覆盖成 ADTS：发送端吐的就是带 ADTS 头的帧
  in_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);
  in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
  in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
  in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

  const HRESULT in_hr = chosen->SetInputType(0, in_type.Get(), 0);
  if (FAILED(in_hr)) {
    DUO_ERROR("AAC 解码器拒绝输入类型: 0x%08X", static_cast<unsigned>(in_hr));
    return Status::NotSupported;
  }

  // 输出：16-bit PCM。让解码器自己给候选。
  ComPtr<IMFMediaType> out_type;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> candidate;
    if (chosen->GetOutputAvailableType(0, i, &candidate) != S_OK) {
      break;
    }
    GUID subtype{};
    if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
        subtype == MFAudioFormat_PCM) {
      out_type = candidate;
      break;
    }
  }
  if (out_type == nullptr) {
    DUO_ERROR("AAC 解码器没有 PCM 输出");
    return Status::NotSupported;
  }
  if (FAILED(chosen->SetOutputType(0, out_type.Get(), 0))) {
    DUO_ERROR("AAC 解码器拒绝 PCM 输出");
    return Status::NotSupported;
  }

  UINT32 rate = 0;
  UINT32 ch = 0;
  out_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  out_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
  impl_->sample_rate = rate != 0 ? rate : 48000;
  impl_->channels = static_cast<u16>(ch != 0 ? ch : 2);

  chosen->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  chosen->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  chosen->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  impl_->transform = chosen;
  impl_->opened = true;
  impl_->next_timestamp = 0;

  DUO_INFO("AAC 解码器就绪: %u Hz, %u 声道", impl_->sample_rate, impl_->channels);
  return Status::Ok;
}

void AacDecoder::close() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->transform != nullptr) {
    impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->transform.Reset();
  }
  impl_->opened = false;
}

bool AacDecoder::is_open() const noexcept { return impl_->opened; }
u32 AacDecoder::sample_rate() const noexcept { return impl_->sample_rate; }
u16 AacDecoder::channels() const noexcept { return impl_->channels; }

Status AacDecoder::decode(ByteSpan aac_adts_frame, std::vector<i16>& out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out.clear();

  if (!impl_->opened || impl_->transform == nullptr) {
    return Status::NotInitialized;
  }
  if (aac_adts_frame.empty()) {
    return Status::InvalidArgument;
  }

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(aac_adts_frame.size()), &buffer))) {
    return Status::IoError;
  }
  BYTE* dst = nullptr;
  if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
    return Status::IoError;
  }
  std::memcpy(dst, aac_adts_frame.data(), aac_adts_frame.size());
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(aac_adts_frame.size()));

  ComPtr<IMFSample> sample;
  if (FAILED(::MFCreateSample(&sample))) {
    return Status::IoError;
  }
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(impl_->next_timestamp);
  sample->SetSampleDuration(10'000'000ll * 1024 / impl_->sample_rate);
  impl_->next_timestamp += 10'000'000ll * 1024 / impl_->sample_rate;

  if (FAILED(impl_->transform->ProcessInput(0, sample.Get(), 0))) {
    return Status::IoError;
  }

  MFT_OUTPUT_STREAM_INFO info{};
  if (FAILED(impl_->transform->GetOutputStreamInfo(0, &info))) {
    return Status::IoError;
  }

  // 必须把解码器里所有可用的输出都取干净。只取一帧就返回的话，
  // 解码器内部缓冲会很快填满，下一次 ProcessInput 直接失败
  // （表现为第 3 帧开始持续 IoError，音频断掉）。
  for (int attempt = 0; attempt < 8; ++attempt) {
    ComPtr<IMFMediaBuffer> out_buf;
    if (FAILED(::MFCreateMemoryBuffer(info.cbSize, &out_buf))) {
      break;
    }
    ComPtr<IMFSample> out_sample;
    if (FAILED(::MFCreateSample(&out_sample))) {
      break;
    }
    out_sample->AddBuffer(out_buf.Get());

    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = 0;
    ob.pSample = out_sample.Get();
    DWORD status = 0;
    const HRESULT hr = impl_->transform->ProcessOutput(0, 1, &ob, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      break;
    }
    if (FAILED(hr)) {
      break;
    }

    BYTE* p = nullptr;
    DWORD len = 0;
    if (SUCCEEDED(out_buf->Lock(&p, nullptr, &len)) && len > 0) {
      const std::size_t samples = len / sizeof(i16);
      const std::size_t old = out.size();
      out.resize(old + samples);
      std::memcpy(out.data() + old, p, len);
    }
    out_buf->Unlock();
  }
  return Status::Ok;  // 需要更多输入是正常的
}

}  // namespace duo

#else

namespace duo {

struct AacDecoder::Impl {};

AacDecoder::AacDecoder() : impl_(std::make_unique<Impl>()) {}
AacDecoder::~AacDecoder() = default;
Status AacDecoder::open() { return Status::NotSupported; }
void AacDecoder::close() {}
Status AacDecoder::decode(ByteSpan, std::vector<i16>&) { return Status::NotSupported; }
bool AacDecoder::is_open() const noexcept { return false; }
u32 AacDecoder::sample_rate() const noexcept { return 0; }
u16 AacDecoder::channels() const noexcept { return 0; }

}  // namespace duo

#endif
