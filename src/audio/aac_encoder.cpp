#include "duocapture/audio/aac_encoder.h"

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
namespace {

// ADTS 头。MKV 的 A_AAC 轨道用 CodecPrivate（AudioSpecificConfig），
// 但很多播放器/推流端更认 ADTS 裸流，这里统一加 ADTS 头。
void append_adts_header(Bytes& out, std::size_t payload_size, u32 sample_rate, u16 channels) {
  static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                               16000, 12000, 11025, 8000,  7350,  0,     0,     0};
  int rate_index = 4;  // 默认 48kHz
  for (int i = 0; i < 13; ++i) {
    if (kRates[i] == static_cast<int>(sample_rate)) {
      rate_index = i;
      break;
    }
  }
  const int profile = 1;  // AAC-LC
  const int chan_cfg = channels == 1 ? 1 : 2;
  const std::size_t frame_len = payload_size + 7;

  out.push_back(0xFF);
  out.push_back(0xF1);  // MPEG-4, Layer 0, no CRC
  out.push_back(static_cast<u8>(((profile & 0x3) << 6) | ((rate_index & 0xF) << 2) |
                                ((chan_cfg >> 2) & 0x1)));
  out.push_back(static_cast<u8>(((chan_cfg & 0x3) << 6) | ((frame_len >> 11) & 0x3)));
  out.push_back(static_cast<u8>((frame_len >> 3) & 0xFF));
  out.push_back(static_cast<u8>(((frame_len & 0x7) << 5) | 0x1F));
  out.push_back(0xFC);
}

}  // namespace

struct AacEncoder::Impl {
  ComPtr<IMFTransform> transform;
  u32 sample_rate = 48000;
  u16 channels = 2;
  u32 bitrate = 128'000;
  bool opened = false;

  std::vector<u8> pending_pcm;  // 不足一帧的残余
  i64 next_timestamp = 0;

  std::mutex mutex;
};

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}

AacEncoder::~AacEncoder() { close(); }

Status AacEncoder::open(u32 sample_rate, u16 channels, u32 bitrate_bps) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (impl_->opened) {
    close();
  }

  static const bool mf_ok = [] {
    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    return SUCCEEDED(hr) || hr == MF_E_ALREADY_INITIALIZED;
  }();
  if (!mf_ok) {
    return Status::NotSupported;
  }

  // 找 AAC 编码器 MFT
  MFT_REGISTER_TYPE_INFO out_info{};
  out_info.guidMajorType = MFMediaType_Audio;
  out_info.guidSubtype = MFAudioFormat_AAC;

  MFT_REGISTER_TYPE_INFO in_info{};
  in_info.guidMajorType = MFMediaType_Audio;
  in_info.guidSubtype = MFAudioFormat_PCM;

  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  const HRESULT hr = ::MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                                 &out_info, &acts, &count);
  if (FAILED(hr) || count == 0) {
    DUO_ERROR("没有可用的 AAC 编码器 MFT");
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

  // 输入：16-bit PCM
  ComPtr<IMFMediaType> in_type;
  if (FAILED(::MFCreateMediaType(&in_type))) {
    return Status::IoError;
  }
  in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
  in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
  in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
  in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * channels * 2);

  if (FAILED(chosen->SetInputType(0, in_type.Get(), 0))) {
    DUO_ERROR("AAC 编码器拒绝输入类型");
    return Status::NotSupported;
  }

  // 输出：AAC
  ComPtr<IMFMediaType> out_type;
  if (FAILED(::MFCreateMediaType(&out_type))) {
    return Status::IoError;
  }
  out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
  out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
  out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate_bps / 8);
  // AAC 特有：payload type 0 = 裸 AAC（ADTS 由我们加），profile 1 = LC
  out_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
  out_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

  if (FAILED(chosen->SetOutputType(0, out_type.Get(), 0))) {
    DUO_ERROR("AAC 编码器拒绝输出类型");
    return Status::NotSupported;
  }

  chosen->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  chosen->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  chosen->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  impl_->transform = chosen;
  impl_->sample_rate = sample_rate;
  impl_->channels = channels;
  impl_->bitrate = bitrate_bps;
  impl_->opened = true;
  impl_->pending_pcm.clear();
  impl_->next_timestamp = 0;

  DUO_INFO("AAC 编码器就绪: %u Hz, %u 声道, %u kbps", sample_rate, channels,
           bitrate_bps / 1000);
  return Status::Ok;
}

void AacEncoder::close() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->transform != nullptr) {
    impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->transform.Reset();
  }
  impl_->opened = false;
  impl_->pending_pcm.clear();
}

bool AacEncoder::is_open() const noexcept { return impl_->opened; }
u32 AacEncoder::sample_rate() const noexcept { return impl_->sample_rate; }
u16 AacEncoder::channels() const noexcept { return impl_->channels; }

Status AacEncoder::encode(ByteSpan pcm_s16, std::vector<Bytes>& out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out.clear();

  if (!impl_->opened || impl_->transform == nullptr) {
    return Status::NotInitialized;
  }

  impl_->pending_pcm.insert(impl_->pending_pcm.end(), pcm_s16.begin(), pcm_s16.end());

  const std::size_t frame_bytes = 1024 * impl_->channels * 2;
  std::size_t offset = 0;

  while (impl_->pending_pcm.size() - offset >= frame_bytes) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(frame_bytes), &buffer))) {
      return Status::IoError;
    }
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
      return Status::IoError;
    }
    std::memcpy(dst, impl_->pending_pcm.data() + offset, frame_bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(frame_bytes));

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
    offset += frame_bytes;

    // 取输出
    MFT_OUTPUT_STREAM_INFO info{};
    impl_->transform->GetOutputStreamInfo(0, &info);
    for (int attempt = 0; attempt < 4; ++attempt) {
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
        Bytes frame;
        append_adts_header(frame, len, impl_->sample_rate, impl_->channels);
        frame.insert(frame.end(), p, p + len);
        out.push_back(std::move(frame));
        out_buf->Unlock();
      } else {
        out_buf->Unlock();
      }
    }
  }

  if (offset > 0) {
    impl_->pending_pcm.erase(impl_->pending_pcm.begin(),
                             impl_->pending_pcm.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return Status::Ok;
}

Status AacEncoder::flush(std::vector<Bytes>& out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out.clear();
  if (!impl_->opened || impl_->transform == nullptr) {
    return Status::NotInitialized;
  }
  impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  return Status::Ok;
}

}  // namespace duo

#else  // !_WIN32

namespace duo {

struct AacEncoder::Impl {};

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}
AacEncoder::~AacEncoder() = default;
Status AacEncoder::open(u32, u16, u32) { return Status::NotSupported; }
void AacEncoder::close() {}
bool AacEncoder::is_open() const noexcept { return false; }
u32 AacEncoder::sample_rate() const noexcept { return 0; }
u16 AacEncoder::channels() const noexcept { return 0; }
Status AacEncoder::encode(ByteSpan, std::vector<Bytes>&) { return Status::NotSupported; }
Status AacEncoder::flush(std::vector<Bytes>&) { return Status::NotSupported; }

}  // namespace duo

#endif
