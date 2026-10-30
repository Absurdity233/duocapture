// D3D11VA 解码器。
//
// 直接用 Windows Media Foundation 的硬件解码 MFT，而不是手写 ID3D11VideoDecoder。
// 原因很实际：手写 DXVA 必须自己解析 SPS/PPS 并构造 DXVA_PicParams_HEVC，
// 那是几千行的规范实现，任何一个字段错了就是花屏或崩溃。MF 的硬件 MFT
// 已经处理了这些，且在所有主流 GPU 上都有实现。
//
// 输出：MF 的解码 MFT 在 D3D11 模式下直接给出 ID3D11Texture2D（NV12），
// 与交换链零拷贝对接。
//
// 限制：
//   * 需要 HEVC 视频扩展（Win10 1809+ 一般通过商店扩展提供；部分系统缺失，
//     此时代码会明确报错而不是静默退化成软解）
//   * 同步模式实现，单线程喂帧

#include "duocapture/decode/decoder.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <mutex>
#include <vector>

#include "duocapture/common/log.h"
#include "duocapture/decode/sps_parser.h"

using Microsoft::WRL::ComPtr;

namespace duo {
namespace {

// 解码器可能在多个线程被调用（收包线程喂帧、呈现线程取纹理），
// 用一把粗锁保护 MF 对象。每帧一次加锁的成本远小于解码本身。
constexpr u32 kMaxDecodeAttempts = 64;

GUID codec_subtype(Codec codec) {
  switch (codec) {
    case Codec::H264: return MFVideoFormat_H264;
    case Codec::Hevc: return MFVideoFormat_HEVC;
    case Codec::Av1: return MFVideoFormat_AV1;
  }
  return MFVideoFormat_HEVC;
}

// 从 Annex-B 码流里抓出 SPS/PPS（H.264）或 VPS/SPS/PPS（HEVC），
// 拼成 MF 需要的 MF_MT_MPEG_SEQUENCE_HEADER blob。
//
// 这一步是必需的：不设这个字段，软件解码 MFT 会在 SetInputType 之后
// 无法确定输出格式，随后 SetOutputType 返回 MF_E_INVALIDMEDIATYPE，
// 报错指向输出，根因却在输入。
std::vector<u8> extract_sequence_header(ByteSpan access_unit, Codec codec) {
  std::vector<u8> out;

  // 先找出所有 NAL 的起止位置（4 字节起始码）。按边界切分，
  // 不要用「往前找下一个起始码」的写法——那种写法在 NAL 尾部紧邻
  // 另一个起始码时会多带几个字节进去，导致后续 SPS 解析错位。
  struct Nal {
    std::size_t begin;
    std::size_t end;
  };
  std::vector<Nal> nals;
  for (std::size_t i = 0; i + 4 <= access_unit.size(); ++i) {
    if (access_unit[i] == 0 && access_unit[i + 1] == 0 && access_unit[i + 2] == 0 &&
        access_unit[i + 3] == 1) {
      nals.push_back({i, access_unit.size()});
      if (nals.size() >= 2) {
        nals[nals.size() - 2].end = i;
      }
      i += 3;
    }
  }

  for (const Nal& nal : nals) {
    if (nal.begin + 4 >= nal.end) {
      continue;
    }
    const u8 header = access_unit[nal.begin + 4];
    bool want = false;
    if (codec == Codec::H264) {
      want = (header == 0x67 || header == 0x68);  // SPS / PPS
    } else if (codec == Codec::Hevc) {
      const u8 type = static_cast<u8>((header >> 1) & 0x3F);
      want = (type == 32 || type == 33 || type == 34);  // VPS / SPS / PPS
    }
    if (!want) {
      continue;
    }
    out.insert(out.end(), access_unit.begin() + static_cast<std::ptrdiff_t>(nal.begin),
               access_unit.begin() + static_cast<std::ptrdiff_t>(nal.end));
  }
  return out;
}

}  // namespace

struct D3d11vaDecoder::Impl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IMFTransform> transform;
  ComPtr<IMFDXGIDeviceManager> device_manager;

  UINT device_manager_token = 0;
  Codec codec = Codec::Hevc;
  bool configured = false;
  bool streaming = false;
  bool is_hardware = false;
  bool use_d3d_manager = false;
  u32 width = 0;
  u32 height = 0;
  i64 next_timestamp = 0;
  // 从首帧 Annex-B 里提取的 SPS/PPS，Configure 输出类型时要设进输入类型。
  std::vector<u8> sequence_header;

  // 软解路径的上传目标纹理，尺寸变化时重建。
  ComPtr<ID3D11Texture2D> upload_target;
  u32 texture_width = 0;
  u32 texture_height = 0;

  u64 frames_decoded = 0;
  u64 frames_dropped_bad_output = 0;

  std::mutex mutex;

  // 当前待输出的纹理。MF 输出的 sample 必须持有到消费者用完为止。
  ComPtr<IMFSample> held_sample;
  ComPtr<ID3D11Texture2D> held_texture;

  void reset() {
    if (transform != nullptr) {
      transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      transform.Reset();
    }
    held_texture.Reset();
    held_sample.Reset();
    if (device_manager != nullptr && device_manager_token != 0) {
      device_manager->ResetDevice(nullptr, device_manager_token);
      device_manager_token = 0;
    }
    device_manager.Reset();
    sequence_header.clear();
    configured = false;
    streaming = false;
  }

  Status create_hw_transform() {
    MFT_REGISTER_TYPE_INFO output_info{};
    output_info.guidMajorType = MFMediaType_Video;
    output_info.guidSubtype = MFVideoFormat_NV12;

    MFT_REGISTER_TYPE_INFO input_info{};
    input_info.guidMajorType = MFMediaType_Video;
    input_info.guidSubtype = codec_subtype(codec);

    IMFActivate** activations = nullptr;
    UINT32 count = 0;

    // 先只找硬件 MFT（NVIDIA/AMD/Intel 的 D3D11VA 解码器）。
    // 找不到再退回 MFT_ENUM_FLAG_ALL —— HEVC 视频扩展提供的是软件 MFT，
    // 在 HARDWARE 标志下不会出现。软解帧率够但 CPU 占用高，所以只作兜底。
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input_info, &output_info,
                             &activations, &count);

    bool hardware = true;
    if (FAILED(hr) || count == 0) {
      DUO_WARN("没有硬件解码器（codec=%d），改用软件解码（CPU 占用会明显升高）",
               static_cast<int>(codec));
      if (activations != nullptr) {
        ::CoTaskMemFree(activations);
        activations = nullptr;
      }
      count = 0;
      // 软件兜底只找同步 MFT。混入 ASYNCMFT 会选到异步实现，而异步 MFT
      // 必须由调用方驱动 METransformNeedInput/Drain 回调，直接同步喂帧
      // 会得到 E_FAIL。
      hr = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input_info,
                       &output_info, &activations, &count);
      hardware = false;
    }

    if (FAILED(hr) || count == 0) {
      DUO_ERROR(
          "未找到解码器（codec=%d）。HEVC 需要安装「HEVC 视频扩展」",
          static_cast<int>(codec));
      return Status::NotSupported;
    }

    // 取第一个可用的。MFTEnumEx 已按硬件优先级排序。
    ComPtr<IMFTransform> chosen;
    for (UINT32 i = 0; i < count; ++i) {
      ComPtr<IMFTransform> candidate;
      if (SUCCEEDED(activations[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
        chosen = candidate;
        break;
      }
    }
    for (UINT32 i = 0; i < count; ++i) {
      activations[i]->Release();
    }
    ::CoTaskMemFree(activations);

    if (chosen == nullptr) {
      return Status::NotSupported;
    }
    transform = chosen;
    is_hardware = hardware;
    DUO_INFO("解码器: %s MFT", hardware ? "硬件" : "软件");
    return Status::Ok;
  }

  Status configure_stream() {
    // 协商顺序：先输入（因为要用 sequence header 设 SPS/PPS），
    // 再枚举并设置输出。解码器会从 SPS 里算出真实分辨率，
    // 我们不去猜尺寸。
    ComPtr<IMFMediaType> fallback_output;
    if (FAILED(::MFCreateMediaType(&fallback_output))) {
      return Status::IoError;
    }
    fallback_output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    fallback_output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

    ComPtr<IMFMediaType> input_type;
    if (FAILED(::MFCreateMediaType(&input_type))) {
      return Status::IoError;
    }
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, codec_subtype(codec));

    // 输入类型必须带上真实分辨率，否则解码器在第一次 ProcessOutput 时
    // 返回 STREAM_CHANGE，而同步喂帧模式下随后的重协商会失败
    // （ProcessInput 报 0xC00D36B5）。
    //
    // 尺寸从 SPS 里解析，不依赖调用方传入——调用方在首帧之前根本拿不到
    // 分辨率（window 尺寸是用户随便给的）。SPS 解析失败时退回调用方给的值。
    const SpsInfo sps = parse_sps_dimensions(sequence_header, codec);
    if (sps.valid) {
      width = sps.width;
      height = sps.height;
      DUO_INFO("SPS 解析分辨率: %ux%u", width, height);
    } else if (width == 0 || height == 0) {
      DUO_WARN("无法确定分辨率（SPS 解析失败且调用方未指定）");
    }
    if (width != 0 && height != 0) {
      ::MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height);
      ::MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, 60, 1);
    }

    // 只设 SUBTYPE，其余属性（帧率、宽高比、交错方式）统统不设。
    //
    // 这不是偷懒：多设属性会让软件 MFT 走到一条不同的推断路径上，
    // 实测表现为它自己算出 1920x1080（而不是码流里的 2560x1600），
    // 随后 ProcessOutput 直接 E_FAIL。真正决定分辨率的是下面这个
    // SPS/PPS blob，解码器会从里面解析出真实尺寸。
    if (!sequence_header.empty()) {
      const HRESULT blob_hr = input_type->SetBlob(
          MF_MT_MPEG_SEQUENCE_HEADER, sequence_header.data(),
          static_cast<UINT32>(sequence_header.size()));
      if (FAILED(blob_hr)) {
        DUO_WARN("设置 MF_MT_MPEG_SEQUENCE_HEADER 失败: 0x%08X", static_cast<unsigned>(blob_hr));
      } else {
        DUO_INFO("已设置 sequence header: %zu 字节", sequence_header.size());
      }
    } else {
      DUO_WARN("未从码流中提取到 SPS/PPS，解码器可能无法初始化");
    }

    // 软件 MFT 只有在输入类型确定后才会列出输出候选，所以先设输入。
    // 硬件 MFT 也接受这个顺序。设完输入再枚举，能拿到真实的可用格式。
    HRESULT hr = transform->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) {
      DUO_ERROR("解码器拒绝输入类型: 0x%08X", static_cast<unsigned>(hr));
      return Status::NotSupported;
    }

    // 枚举解码器自己给出的输出候选。
    ComPtr<IMFMediaType> preferred_output;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> candidate;
      if (transform->GetOutputAvailableType(0, i, &candidate) != S_OK) {
        break;
      }
      GUID subtype{};
      if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        UINT32 w = 0;
        UINT32 h = 0;
        ::MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &w, &h);
        DUO_DEBUG("解码器输出候选 [%u]: {%08X-%04X-%04X} w=%u h=%u", i, subtype.Data1,
                  subtype.Data2, subtype.Data3, w, h);
        if (subtype == MFVideoFormat_NV12 && preferred_output == nullptr) {
          preferred_output = candidate;
        }
      }
    }

    ComPtr<IMFMediaType> try_output;
    if (preferred_output != nullptr) {
      // 直接用解码器给出的候选，一个字节都不改。往候选里塞额外属性
      // 会让部分 MFT 认为类型不匹配。
      hr = transform->SetOutputType(0, preferred_output.Get(), 0);
      if (SUCCEEDED(hr)) {
        try_output = preferred_output;
      }
    }
    if (try_output == nullptr) {
      hr = transform->SetOutputType(0, fallback_output.Get(), 0);
      try_output = fallback_output;
    }

    if (FAILED(hr)) {
      DUO_ERROR("解码器拒绝输出类型: 0x%08X（d3d_manager=%d）", static_cast<unsigned>(hr),
                use_d3d_manager ? 1 : 0);
      return Status::NotSupported;
    }

    {
      UINT32 w = 0;
      UINT32 h = 0;
      if (SUCCEEDED(::MFGetAttributeSize(try_output.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
        width = w;
        height = h;
      }
    }

    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    streaming = true;
    configured = true;

    DUO_INFO("D3D11VA 解码器就绪: %ux%u", width, height);
    return Status::Ok;
  }

  Status upload_nv12(IMFMediaBuffer* source, ComPtr<ID3D11Texture2D>& texture) {
  if (device == nullptr) {
    return Status::NotInitialized;
  }

  const u32 w = width;
  const u32 h = height;
  if (w == 0 || h == 0) {
    return Status::NotInitialized;
  }

  if (texture == nullptr || texture_width != w || texture_height != h) {
    texture.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()))) {
      return Status::NotSupported;
    }
    texture_width = w;
    texture_height = h;
  }

  BYTE* data = nullptr;
  DWORD length = 0;
  if (FAILED(source->Lock(&data, nullptr, &length))) {
    return Status::IoError;
  }

  // 系统内存 NV12 的跨度是连续排布的：Y 平面 w*h，其后是 UV 平面 w*h/2。
  const std::size_t expected = static_cast<std::size_t>(w) * h * 3 / 2;
  if (length < expected) {
    source->Unlock();
    return Status::IoError;
  }

  context->UpdateSubresource(texture.Get(), 0, nullptr, data, w, 0);
  source->Unlock();
  return Status::Ok;
}

};

D3d11vaDecoder::D3d11vaDecoder() : impl_(std::make_unique<Impl>()) {}

D3d11vaDecoder::~D3d11vaDecoder() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->reset();
}

Status D3d11vaDecoder::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device == nullptr) {
    return Status::InvalidArgument;
  }

  static bool mf_started = [] {
    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    return SUCCEEDED(hr);
  }();
  if (!mf_started) {
    return Status::NotSupported;
  }

  impl_->device = device;
  impl_->context = context;

  ComPtr<IMFDXGIDeviceManager> manager;
  UINT token = 0;
  if (FAILED(::MFCreateDXGIDeviceManager(&token, &manager))) {
    return Status::NotSupported;
  }
  if (FAILED(manager->ResetDevice(device, token))) {
    return Status::NotSupported;
  }
  impl_->device_manager = manager;
  impl_->device_manager_token = token;
  return Status::Ok;
}

Status D3d11vaDecoder::configure(Codec codec, ByteSpan first_access_unit) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (impl_->device_manager == nullptr) {
    return Status::NotInitialized;
  }

  impl_->codec = codec;
  impl_->reset();

  // 从首帧提取 SPS/PPS，供 configure_stream 构造输入类型。
  impl_->sequence_header = extract_sequence_header(first_access_unit, codec);
  DUO_INFO("首帧 %zu 字节，sequence header %zu 字节", first_access_unit.size(),
           impl_->sequence_header.size());


  Status status = impl_->create_hw_transform();
  if (!ok(status)) {
    return status;
  }

  // 绑定 D3D11 设备管理器，让解码器在 GPU 上工作并输出 D3D11 纹理。
  //
  // 只对硬件 MFT 发送这条消息。软件 MFT 常常也「成功」返回，但之后会
  // 进入一种半吊子状态：既不再输出系统内存样本，又给不出 D3D11 纹理，
  // 表现为后续 SetOutputType 返回 MF_E_INVALIDMEDIATYPE。按类型判断，
  // 而不是按返回值判断。
  if (impl_->is_hardware) {
    if (impl_->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                         reinterpret_cast<ULONG_PTR>(
                                             impl_->device_manager.Get())) == S_OK) {
      impl_->use_d3d_manager = true;
    } else {
      DUO_WARN("硬件解码器拒绝 D3D11 设备管理器，改用系统内存输出");
      impl_->use_d3d_manager = false;
    }
  } else {
    impl_->use_d3d_manager = false;
  }

  return impl_->configure_stream();
}

Status D3d11vaDecoder::decode(ByteSpan access_unit, u64 capture_ts_ns, DecodedFrame& out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (!impl_->configured || impl_->transform == nullptr) {
    return Status::NotInitialized;
  }
  if (access_unit.empty()) {
    return Status::InvalidArgument;
  }

  // 上一帧的纹理如果没人释放，就在这里替它释放。呈现端应当已经 release 过。
  impl_->held_texture.Reset();
  impl_->held_sample.Reset();

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(access_unit.size()), &buffer))) {
    return Status::IoError;
  }

  BYTE* dst = nullptr;
  if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
    return Status::IoError;
  }
  std::memcpy(dst, access_unit.data(), access_unit.size());
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(access_unit.size()));

  ComPtr<IMFSample> sample;
  if (FAILED(::MFCreateSample(&sample))) {
    return Status::IoError;
  }
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(impl_->next_timestamp);
  sample->SetSampleDuration(10'000'000 / 60);
  impl_->next_timestamp += 10'000'000 / 60;

  const HRESULT input_hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
  if (FAILED(input_hr)) {
    DUO_ERROR("解码器 ProcessInput 失败: 0x%08X", static_cast<unsigned>(input_hr));
    return Status::IoError;
  }

  // 同步模式：反复 ProcessOutput 直到拿不到新帧
  MFT_OUTPUT_STREAM_INFO stream_info{};
  if (FAILED(impl_->transform->GetOutputStreamInfo(0, &stream_info))) {
    return Status::IoError;
  }

  for (u32 attempt = 0; attempt < kMaxDecodeAttempts; ++attempt) {
    ComPtr<IMFSample> output_sample;
    MFT_OUTPUT_DATA_BUFFER output_buffer{};
    output_buffer.dwStreamID = 0;

    // 只有 MFT_OUTPUT_STREAM_PROVIDES_SAMPLES 才表示解码器「一定」自己分配
    // 输出 sample，此时 pSample 必须为 NULL。CAN_PROVIDE_SAMPLES 只是说
    // 它「能」分配，但要求调用方依然传入自分配的 sample——把两者混为一谈
    // 会让 ProcessOutput 直接返回 E_FAIL。
    const bool mft_provides_samples =
        (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    if (!mft_provides_samples) {
      if (FAILED(::MFCreateSample(&output_sample))) {
        return Status::IoError;
      }
      ComPtr<IMFMediaBuffer> out_buffer;
      if (FAILED(::MFCreateMemoryBuffer(stream_info.cbSize, &out_buffer))) {
        return Status::IoError;
      }
      output_sample->AddBuffer(out_buffer.Get());
      output_buffer.pSample = output_sample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = impl_->transform->ProcessOutput(0, 1, &output_buffer, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      break;  // 这一帧还没解出完整图像，等下一个输入
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // 分辨率或格式变了，重新协商输出类型
      ComPtr<IMFMediaType> new_type;
      for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> candidate;
        if (impl_->transform->GetOutputAvailableType(0, i, &candidate) != S_OK) {
          break;
        }
        GUID subtype{};
        if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_NV12) {
          new_type = candidate;
          break;
        }
      }
      if (new_type == nullptr) {
        return Status::NotSupported;
      }
      impl_->transform->SetOutputType(0, new_type.Get(), 0);
      UINT32 w = 0;
      UINT32 h = 0;
      if (SUCCEEDED(::MFGetAttributeSize(new_type.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
        impl_->width = w;
        impl_->height = h;
      }
      continue;
    }
    if (FAILED(hr)) {
      DUO_ERROR("解码器 ProcessOutput 失败: 0x%08X", static_cast<unsigned>(hr));
      return Status::IoError;
    }

    IMFSample* produced = output_buffer.pSample;
    if (produced == nullptr) {
      continue;
    }

    // 从 MF 的缓冲里取纹理。硬件 MFT 给出的是 DXGI 缓冲（可直接取
    // ID3D11Texture2D，零拷贝）；软件 MFT 给的是系统内存 NV12，
    // 需要自己拷进一张 D3D11 纹理才能呈现。
    ComPtr<IMFMediaBuffer> out_buffer;
    if (FAILED(produced->GetBufferByIndex(0, &out_buffer))) {
      ++impl_->frames_dropped_bad_output;
      continue;
    }

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    if (SUCCEEDED(out_buffer.As(&dxgi_buffer))) {
      if (FAILED(dxgi_buffer->GetResource(IID_PPV_ARGS(&texture)))) {
        ++impl_->frames_dropped_bad_output;
        continue;
      }
    } else {
      // 软件解码路径：CPU 内存 → staging → 纹理
      Status upload = impl_->upload_nv12(out_buffer.Get(), texture);
      if (!ok(upload)) {
        ++impl_->frames_dropped_bad_output;
        continue;
      }
    }

    impl_->held_sample = produced;
    impl_->held_texture = texture;

    out.texture = texture.Get();
    out.width = impl_->width;
    out.height = impl_->height;
    out.capture_ts_ns = capture_ts_ns;
    out.key = false;
    ++impl_->frames_decoded;
    return Status::Ok;
  }

  return Status::WouldBlock;  // 输入已接收，但还没有可呈现的帧
}

// 把系统内存里的 NV12 样本上传到一张 D3D11 纹理。
// 这是软解路径的代价：一次完整的 CPU→GPU 拷贝（2560x1600 NV12 约 6 MB）。

void D3d11vaDecoder::release(DecodedFrame& frame) {
  frame.texture = nullptr;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->held_texture.Reset();
  impl_->held_sample.Reset();
}

const char* D3d11vaDecoder::backend_name() const { return "d3d11va-mft"; }

std::unique_ptr<IDecoder> create_decoder() { return std::make_unique<D3d11vaDecoder>(); }

}  // namespace duo

#else

namespace duo {

std::unique_ptr<IDecoder> create_decoder() { return nullptr; }

}  // namespace duo

#endif
