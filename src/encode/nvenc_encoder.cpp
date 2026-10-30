#include "duocapture/encode/nvenc_encoder.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "nvEncodeAPI.h"

namespace duo {
namespace {

constexpr GUID kCodecGuid(Codec codec) noexcept {
  switch (codec) {
    case Codec::H264: return NV_ENC_CODEC_H264_GUID;
    case Codec::Hevc: return NV_ENC_CODEC_HEVC_GUID;
    case Codec::Av1: return NV_ENC_CODEC_AV1_GUID;
  }
  return NV_ENC_CODEC_HEVC_GUID;
}

const char* codec_guid_name(Codec codec) noexcept {
  switch (codec) {
    case Codec::H264: return "h264";
    case Codec::Hevc: return "hevc";
    case Codec::Av1: return "av1";
  }
  return "unknown";
}

// 从驱动取预设配置，再覆盖成低延迟参数。
//
// 关键：不能自己从零填 NV_ENC_CONFIG。驱动会拒绝任何字段布局与它期望
// 不一致的结构体，报出来是 NV_ENC_ERR_INVALID_PARAM，很容易误判成参数
// 取值问题。正确做法是让驱动给出预设，然后只改需要改的字段。
void apply_low_latency_overrides(NV_ENC_CONFIG& config, const EncoderConfig& cfg) {
  config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
  config.gopLength = cfg.gop_frames == 0 ? NVENC_INFINITE_GOPLENGTH : cfg.gop_frames;
  config.frameIntervalP = 1;  // 禁 B 帧，否则解码端要等后续帧才能输出

  config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  config.rcParams.averageBitRate = cfg.bitrate_bps;
  config.rcParams.maxBitRate = cfg.bitrate_bps;
  config.rcParams.zeroReorderDelay = 1;
  config.rcParams.enableLookahead = 0;

  // VBV 设为 1 帧的码量（单位是 bit）。这是整条链路上最容易被忽略的
  // 参数：默认值会让编码器攒 2-3 帧再吐码，白白加 20-30 ms。
  const u32 frame_bits = cfg.fps == 0 ? cfg.bitrate_bps / 120 : cfg.bitrate_bps / cfg.fps;
  config.rcParams.vbvBufferSize = frame_bits;
  config.rcParams.vbvInitialDelay = frame_bits;
}

}  // namespace

struct NvencEncoder::Impl {
  // 直接持有官方函数表，所有调用都经过它——手写函数指针在 x64 的
  // NVENCAPI 调用约定上很容易出错，且字段名会随版本变化。
  NV_ENCODE_API_FUNCTION_LIST fn{};
  HMODULE module = nullptr;
  // 驱动实际支持的 NVENC API 版本。编译期头文件版本可能比它新，
  // 直接拿头文件版本去开会话会得到 NV_ENC_ERR_INVALID_VERSION(15)。
  u32 driver_api_version = 0;

  void* encoder = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  NV_ENC_OUTPUT_PTR bitstream = nullptr;
  NV_ENC_REGISTERED_PTR registered_texture = nullptr;
  NV_ENC_INPUT_PTR mapped_input = nullptr;
  u32 mapped_width = 0;
  u32 mapped_height = 0;
  DXGI_FORMAT mapped_format = DXGI_FORMAT_UNKNOWN;

  EncoderConfig config{};
  Stats stats{};
  bool annex_b = true;
  bool configured = false;
  bool force_keyframe = true;
  u32 frame_index = 0;

  Status load_library() {
    if (module != nullptr) {
      return Status::Ok;
    }
    module = ::LoadLibraryExA("nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      DUO_ERROR("nvEncodeAPI64.dll 未找到（需要 NVIDIA 显卡驱动）");
      return Status::NotSupported;
    }

    auto create_instance = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(
        ::GetProcAddress(module, "NvEncodeAPICreateInstance"));
    if (create_instance == nullptr) {
      DUO_ERROR("nvEncodeAPI64.dll 缺少 NvEncodeAPICreateInstance");
      ::FreeLibrary(module);
      module = nullptr;
      return Status::NotSupported;
    }

    auto get_max_version = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(uint32_t*)>(
        ::GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
    if (get_max_version != nullptr) {
      uint32_t version = 0;
      if (get_max_version(&version) == NV_ENC_SUCCESS) {
        // GetMaxSupportedVersion 返回：低 4 位 minor，其余位 major。
        // NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS::apiVersion 要的是
        // NVENCAPI_VERSION 的打包方式（major | minor << 24），两者不同，
        // 必须显式转换。
        const u32 major = version >> 4;
        const u32 minor = version & 0xF;
        driver_api_version = major | (minor << 24);
        DUO_INFO("NVENC 驱动 API 版本: %u.%u（头文件 %u.%u）", major, minor,
                 static_cast<unsigned>(NVENCAPI_MAJOR_VERSION),
                 static_cast<unsigned>(NVENCAPI_MINOR_VERSION));
      }
    }
    if (driver_api_version == 0) {
      driver_api_version = NVENCAPI_VERSION;
    }

    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const NVENCSTATUS status = create_instance(&fn);
    if (status != NV_ENC_SUCCESS) {
      DUO_ERROR("NvEncodeAPICreateInstance 失败: %d", static_cast<int>(status));
      ::FreeLibrary(module);
      module = nullptr;
      return Status::NotSupported;
    }
    return Status::Ok;
  }

  void unmap() {
    if (encoder != nullptr && mapped_input != nullptr) {
      fn.nvEncUnmapInputResource(encoder, mapped_input);
      mapped_input = nullptr;
    }
  }

  void teardown() {
    if (encoder != nullptr) {
      unmap();
      if (registered_texture != nullptr) {
        fn.nvEncUnregisterResource(encoder, registered_texture);
        registered_texture = nullptr;
      }
      if (bitstream != nullptr) {
        fn.nvEncDestroyBitstreamBuffer(encoder, bitstream);
        bitstream = nullptr;
      }
      fn.nvEncDestroyEncoder(encoder);
      encoder = nullptr;
    }
    mapped_width = 0;
    mapped_height = 0;
    mapped_format = DXGI_FORMAT_UNKNOWN;
    configured = false;
  }

  void release_library() {
    if (module != nullptr) {
      ::FreeLibrary(module);
      module = nullptr;
    }
  }
};

NvencEncoder::NvencEncoder() : impl_(std::make_unique<Impl>()) {}

NvencEncoder::~NvencEncoder() {
  impl_->teardown();
  impl_->release_library();
}

Status NvencEncoder::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device == nullptr) {
    return Status::InvalidArgument;
  }
  impl_->device = device;
  impl_->context = context;
  return impl_->load_library();
}

Status NvencEncoder::configure(const EncoderConfig& config) {
  if (config.width == 0 || config.height == 0 || config.fps == 0) {
    return Status::InvalidArgument;
  }
  if (impl_->module == nullptr) {
    return Status::NotInitialized;
  }

  impl_->teardown();
  impl_->config = config;

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
  session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  session.device = impl_->device;
  session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  // 用驱动支持的最高版本，而不是头文件版本。
  session.apiVersion = impl_->driver_api_version != 0 ? impl_->driver_api_version
                                                      : NVENCAPI_VERSION;

  NVENCSTATUS status = impl_->fn.nvEncOpenEncodeSessionEx(&session, &impl_->encoder);
  if (status != NV_ENC_SUCCESS) {
    DUO_ERROR("NVENC 打开会话失败: %d", static_cast<int>(status));
    impl_->encoder = nullptr;
    return Status::NotSupported;
  }

  // 让驱动生成预设配置，再覆盖成低延迟参数。
  NV_ENC_PRESET_CONFIG preset{};
  preset.version = NV_ENC_PRESET_CONFIG_VER;
  preset.presetCfg.version = NV_ENC_CONFIG_VER;
  status = impl_->fn.nvEncGetEncodePresetConfigEx(
      impl_->encoder, kCodecGuid(config.codec), NV_ENC_PRESET_P1_GUID,
      NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
  if (status != NV_ENC_SUCCESS) {
    DUO_ERROR("NVENC 取预设配置失败: %d", static_cast<int>(status));
    impl_->teardown();
    return Status::NotSupported;
  }

  NV_ENC_CONFIG encoder_config = preset.presetCfg;
  apply_low_latency_overrides(encoder_config, config);

  switch (config.codec) {
    case Codec::H264: {
      auto& h264 = encoder_config.encodeCodecConfig.h264Config;
      h264.idrPeriod = config.gop_frames == 0 ? NVENC_INFINITE_GOPLENGTH : config.gop_frames;
      h264.repeatSPSPPS = 1;
      h264.sliceMode = 3;
      h264.sliceModeData = config.slice_count == 0 ? 4 : config.slice_count;
      if (config.intra_refresh) {
        h264.enableIntraRefresh = 1;
        // Period 是两次刷新之间的间隔，Cnt 是刷新完整一屏所需帧数。
        // 两者相等会让每帧都刷新整屏，退化成"每帧都是 I 帧"，码率翻倍。
        h264.intraRefreshPeriod = config.fps * 2;   // 每 2 秒一轮
        h264.intraRefreshCnt = config.fps / 2;      // 0.5 秒刷完一屏
      }
      break;
    }
    case Codec::Hevc: {
      auto& hevc = encoder_config.encodeCodecConfig.hevcConfig;
      hevc.idrPeriod = config.gop_frames == 0 ? NVENC_INFINITE_GOPLENGTH : config.gop_frames;
      hevc.repeatSPSPPS = 1;
      hevc.sliceMode = 3;
      hevc.sliceModeData = config.slice_count == 0 ? 4 : config.slice_count;
      if (config.intra_refresh) {
        hevc.enableIntraRefresh = 1;
        // Period 是两次刷新之间的间隔，Cnt 是刷新完整一屏所需帧数。
        // 两者相等会让每帧都刷新整屏，退化成"每帧都是 I 帧"，码率翻倍。
        hevc.intraRefreshPeriod = config.fps * 2;   // 每 2 秒一轮
        hevc.intraRefreshCnt = config.fps / 2;      // 0.5 秒刷完一屏
      }
      break;
    }
    case Codec::Av1: {
      auto& av1 = encoder_config.encodeCodecConfig.av1Config;
      av1.idrPeriod = config.gop_frames == 0 ? NVENC_INFINITE_GOPLENGTH : config.gop_frames;
      av1.repeatSeqHdr = 1;
      if (config.intra_refresh) {
        av1.enableIntraRefresh = 1;
        // Period 是两次刷新之间的间隔，Cnt 是刷新完整一屏所需帧数。
        // 两者相等会让每帧都刷新整屏，退化成"每帧都是 I 帧"，码率翻倍。
        av1.intraRefreshPeriod = config.fps * 2;   // 每 2 秒一轮
        av1.intraRefreshCnt = config.fps / 2;      // 0.5 秒刷完一屏
      }
      break;
    }
  }

  NV_ENC_INITIALIZE_PARAMS params{};
  params.version = NV_ENC_INITIALIZE_PARAMS_VER;
  params.encodeGUID = kCodecGuid(config.codec);
  params.presetGUID = NV_ENC_PRESET_P1_GUID;
  params.encodeWidth = config.width;
  params.encodeHeight = config.height;
  params.darWidth = config.width;
  params.darHeight = config.height;
  params.frameRateNum = config.fps;
  params.frameRateDen = 1;
  // 同步模式。异步模式（enableEncodeAsync=1）要求调用方注册完成事件并
  // 自行驱动完成回调，否则 nvEncEncodePicture 直接返回 INVALID_PARAM。
  // 对低延迟管线来说同步反而更合适：编码器内部已经流水化，同步调用只会
  // 阻塞到本帧完成，不会引入额外排队。
  params.enableEncodeAsync = 0;
  params.enablePTD = 1;
  params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  params.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
  params.encodeConfig = &encoder_config;
  params.maxEncodeWidth = config.width;
  params.maxEncodeHeight = config.height;

  status = impl_->fn.nvEncInitializeEncoder(impl_->encoder, &params);
  if (status != NV_ENC_SUCCESS) {
    DUO_ERROR("NVENC 初始化失败: %d（%s %ux%u @%u）", static_cast<int>(status),
              codec_guid_name(config.codec), config.width, config.height, config.fps);
    impl_->teardown();
    return Status::NotSupported;
  }

  NV_ENC_CREATE_BITSTREAM_BUFFER create{};
  create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  create.size = static_cast<u32>(config.bitrate_bps / 8 / std::max(1u, config.fps) * 2 + 65536);
  status = impl_->fn.nvEncCreateBitstreamBuffer(impl_->encoder, &create);
  if (status != NV_ENC_SUCCESS) {
    impl_->teardown();
    return Status::NotSupported;
  }
  impl_->bitstream = create.bitstreamBuffer;

  // H.264 / HEVC 的 NVENC 输出固定为 Annex-B；AV1 输出带 OBU 的裸流。
  impl_->annex_b = config.codec != Codec::Av1;
  impl_->configured = true;
  impl_->force_keyframe = true;
  impl_->frame_index = 0;
  impl_->stats = Stats{};

  DUO_INFO("NVENC 就绪: %s %ux%u @%u, CBR %u Mbps, intraRefresh=%d, annexB=%d",
           codec_guid_name(config.codec), config.width, config.height, config.fps,
           config.bitrate_bps / 1000000, config.intra_refresh ? 1 : 0, impl_->annex_b ? 1 : 0);
  return Status::Ok;
}

Status NvencEncoder::encode(const FrameView& frame, EncodedFrame& out) {
  if (!impl_->configured || impl_->encoder == nullptr) {
    return Status::NotInitialized;
  }
  if (frame.texture == nullptr) {
    return Status::InvalidArgument;
  }

  auto* texture = static_cast<ID3D11Texture2D*>(frame.texture);
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);

  // 输入纹理必须是 NV12。尺寸或格式变了就重新注册并映射。
  const bool need_register = impl_->mapped_input == nullptr ||
                             impl_->mapped_width != desc.Width ||
                             impl_->mapped_height != desc.Height ||
                             impl_->mapped_format != desc.Format;

  if (need_register) {
    impl_->unmap();
    if (impl_->registered_texture != nullptr) {
      impl_->fn.nvEncUnregisterResource(impl_->encoder, impl_->registered_texture);
      impl_->registered_texture = nullptr;
    }

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = desc.Width;
    reg.height = desc.Height;
    reg.pitch = 0;
    reg.resourceToRegister = texture;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;

    const NVENCSTATUS reg_status = impl_->fn.nvEncRegisterResource(impl_->encoder, &reg);
    if (reg_status != NV_ENC_SUCCESS) {
      DUO_ERROR("NVENC 注册纹理失败: %d（输入必须是 NV12 且尺寸为偶数）",
                static_cast<int>(reg_status));
      return Status::NotSupported;
    }
    impl_->registered_texture = reg.registeredResource;

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = impl_->registered_texture;
    const NVENCSTATUS map_status = impl_->fn.nvEncMapInputResource(impl_->encoder, &map);
    if (map_status != NV_ENC_SUCCESS) {
      DUO_ERROR("NVENC 映射纹理失败: %d", static_cast<int>(map_status));
      return Status::NotSupported;
    }

    impl_->mapped_input = map.mappedResource;
    impl_->mapped_width = desc.Width;
    impl_->mapped_height = desc.Height;
    impl_->mapped_format = desc.Format;
  }

  const u64 begin = Clock::now_ns();

  NV_ENC_PIC_PARAMS pic{};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputWidth = impl_->config.width;
  pic.inputHeight = impl_->config.height;
  // DirectX 输入忽略 pitch，置 0 即可。
  pic.inputPitch = 0;
  pic.inputBuffer = impl_->mapped_input;
  pic.outputBitstream = impl_->bitstream;
  pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.frameIdx = impl_->frame_index++;
  pic.inputTimeStamp = frame.capture_ts_ns;
  if (impl_->force_keyframe) {
    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    impl_->force_keyframe = false;
  }

  const NVENCSTATUS encode_status = impl_->fn.nvEncEncodePicture(impl_->encoder, &pic);
  if (encode_status != NV_ENC_SUCCESS) {
    DUO_ERROR("NVENC 编码失败: %d", static_cast<int>(encode_status));
    return Status::IoError;
  }
  ++impl_->stats.frames_submitted;

  NV_ENC_LOCK_BITSTREAM lock{};
  lock.version = NV_ENC_LOCK_BITSTREAM_VER;
  lock.outputBitstream = impl_->bitstream;
  lock.doNotWait = 0;
  if (impl_->fn.nvEncLockBitstream(impl_->encoder, &lock) != NV_ENC_SUCCESS) {
    return Status::IoError;
  }

  out.data.assign(static_cast<const u8*>(lock.bitstreamBufferPtr),
                  static_cast<const u8*>(lock.bitstreamBufferPtr) + lock.bitstreamSizeInBytes);
  out.key = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
  out.capture_ts_ns = frame.capture_ts_ns;

  impl_->fn.nvEncUnlockBitstream(impl_->encoder, impl_->bitstream);

  const double elapsed_ms = Clock::to_ms(Clock::now_ns() - begin);
  ++impl_->stats.frames_encoded;
  impl_->stats.bytes_out += out.data.size();
  impl_->stats.avg_encode_ms =
      (impl_->stats.avg_encode_ms * static_cast<double>(impl_->stats.frames_encoded - 1) +
       elapsed_ms) /
      static_cast<double>(impl_->stats.frames_encoded);
  return Status::Ok;
}

Status NvencEncoder::request_keyframe() {
  if (!impl_->configured) {
    return Status::NotInitialized;
  }
  impl_->force_keyframe = true;
  return Status::Ok;
}

const char* NvencEncoder::backend_name() const { return "nvenc"; }

const NvencEncoder::Stats& NvencEncoder::stats() const noexcept { return impl_->stats; }

const EncoderConfig& NvencEncoder::config() const noexcept { return impl_->config; }

bool NvencEncoder::outputs_annex_b() const noexcept { return impl_->annex_b; }

}  // namespace duo

#else  // !_WIN32

namespace duo {

NvencEncoder::NvencEncoder() : impl_(std::make_unique<Impl>()) {}
NvencEncoder::~NvencEncoder() = default;

}  // namespace duo

#endif  // _WIN32
