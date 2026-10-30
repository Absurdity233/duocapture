// NVDEC 硬件解码（CUVID 直连）。
//
// 不走 Media Foundation 的原因见头文件。这里的关键实现点：
//
//  * nvcuvid.dll 新版直接导出 cuvid* 函数，没有 cuvidCreateInstance。
//    参数顺序务必按头文件：cuvidCreateDecoder(&decoder, &createInfo)，
//    句柄在前、参数在后。写反了会得到 CUDA_ERROR_INVALID_VALUE(1)，
//    看起来像"硬件不支持"，实际只是参数错位。
//
//  * CUDA 上下文可以用普通 cuCtxCreate_v2。D3D11 互操作
//    （cuD3D11CtxCreate_v2）能省一次显存拷贝，但它依赖 adapter 与
//    CUDA 设备的对应关系，在混合显卡笔记本上经常拿不到设备号。
//    这里做成可选：互操作可用就用，否则退回 CUDA 内存 → 上传纹理。
//
//  * CUVID 的回调是同步的：cuvidParseVideoData 返回时，pfnDecodePicture
//    和 pfnDisplayPicture 都已经执行完。所以解码结果可以在 decode() 里
//    直接取走。

#include "duocapture/decode/nvdec_decoder.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "duocapture/decode/sps_parser.h"

// nv-codec-headers 的 dynlink_* 只提供类型定义，函数原型要自己声明。
#include "dynlink_cuda.h"
#include "dynlink_cuviddec.h"
#include "dynlink_nvcuvid.h"

using Microsoft::WRL::ComPtr;

namespace duo {
namespace {

// ---- 动态解析出来的函数表 ------------------------------------------------
struct CudaApi {
  HMODULE module = nullptr;

  CUresult (*init)(unsigned) = nullptr;
  CUresult (*device_get_count)(int*) = nullptr;
  CUresult (*device_get)(CUdevice*, int) = nullptr;
  CUresult (*ctx_create)(CUcontext*, unsigned, CUdevice) = nullptr;
  CUresult (*ctx_destroy)(CUcontext) = nullptr;
  CUresult (*ctx_push_current)(CUcontext) = nullptr;
  CUresult (*ctx_pop_current)(CUcontext*) = nullptr;
  CUresult (*mem_alloc)(CUdeviceptr*, std::size_t) = nullptr;
  CUresult (*mem_free)(CUdeviceptr) = nullptr;
  CUresult (*memcpy_dtoh)(void*, CUdeviceptr, std::size_t) = nullptr;
  CUresult (*memcpy_dtod)(CUdeviceptr, CUdeviceptr, std::size_t) = nullptr;
  CUresult (*stream_synchronize)(CUstream) = nullptr;

  // D3D11 互操作（可选）
  CUresult (*d3d11_ctx_create)(CUcontext*, unsigned, CUdevice, IDXGIAdapter*, unsigned,
                               void*) = nullptr;
  CUresult (*d3d11_get_devices)(unsigned*, CUdevice*, unsigned, IDXGIAdapter*) = nullptr;
  CUresult (*graphics_d3d11_register_resource)(CUgraphicsResource*, void*, unsigned) = nullptr;
  CUresult (*graphics_map_resources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
  CUresult (*graphics_unmap_resources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
  CUresult (*graphics_get_mapped_pointer)(CUdeviceptr*, std::size_t*, CUgraphicsResource) =
      nullptr;
  CUresult (*graphics_unregister_resource)(CUgraphicsResource) = nullptr;

  bool load() {
    if (module != nullptr) {
      return true;
    }
    module = ::LoadLibraryExA("nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      return false;
    }

    auto resolve = [this](const char* name) -> void* {
      return reinterpret_cast<void*>(::GetProcAddress(module, name));
    };

    init = reinterpret_cast<decltype(init)>(resolve("cuInit"));
    device_get_count = reinterpret_cast<decltype(device_get_count)>(resolve("cuDeviceGetCount"));
    device_get = reinterpret_cast<decltype(device_get)>(resolve("cuDeviceGet"));
    ctx_create = reinterpret_cast<decltype(ctx_create)>(resolve("cuCtxCreate_v2"));
    ctx_destroy = reinterpret_cast<decltype(ctx_destroy)>(resolve("cuCtxDestroy_v2"));
    ctx_push_current = reinterpret_cast<decltype(ctx_push_current)>(resolve("cuCtxPushCurrent_v2"));
    ctx_pop_current = reinterpret_cast<decltype(ctx_pop_current)>(resolve("cuCtxPopCurrent_v2"));
    mem_alloc = reinterpret_cast<decltype(mem_alloc)>(resolve("cuMemAlloc_v2"));
    mem_free = reinterpret_cast<decltype(mem_free)>(resolve("cuMemFree_v2"));
    memcpy_dtoh = reinterpret_cast<decltype(memcpy_dtoh)>(resolve("cuMemcpyDtoH_v2"));
    memcpy_dtod = reinterpret_cast<decltype(memcpy_dtod)>(resolve("cuMemcpyDtoD_v2"));
    stream_synchronize =
        reinterpret_cast<decltype(stream_synchronize)>(resolve("cuStreamSynchronize"));

    d3d11_ctx_create =
        reinterpret_cast<decltype(d3d11_ctx_create)>(resolve("cuD3D11CtxCreate_v2"));
    d3d11_get_devices =
        reinterpret_cast<decltype(d3d11_get_devices)>(resolve("cuD3D11GetDevices_v2"));
    graphics_d3d11_register_resource = reinterpret_cast<decltype(graphics_d3d11_register_resource)>(
        resolve("cuGraphicsD3D11RegisterResource"));
    graphics_map_resources =
        reinterpret_cast<decltype(graphics_map_resources)>(resolve("cuGraphicsMapResources"));
    graphics_unmap_resources =
        reinterpret_cast<decltype(graphics_unmap_resources)>(resolve("cuGraphicsUnmapResources"));
    graphics_get_mapped_pointer = reinterpret_cast<decltype(graphics_get_mapped_pointer)>(
        resolve("cuGraphicsResourceGetMappedPointer"));
    graphics_unregister_resource = reinterpret_cast<decltype(graphics_unregister_resource)>(
        resolve("cuGraphicsUnregisterResource"));

    return init != nullptr && ctx_create != nullptr;
  }

  bool has_d3d11_interop() const noexcept {
    return d3d11_ctx_create != nullptr && graphics_d3d11_register_resource != nullptr;
  }

  void unload() {
    if (module != nullptr) {
      ::FreeLibrary(module);
      module = nullptr;
    }
  }
};

struct CuvidApi {
  HMODULE module = nullptr;

  CUresult (*create_decoder)(CUvideodecoder*, CUVIDDECODECREATEINFO*) = nullptr;
  CUresult (*destroy_decoder)(CUvideodecoder) = nullptr;
  CUresult (*decode_picture)(CUvideodecoder, CUVIDPICPARAMS*) = nullptr;
  CUresult (*map_frame64)(CUvideodecoder, int, unsigned long long*, unsigned int*,
                          CUVIDPROCPARAMS*) = nullptr;
  CUresult (*unmap_frame64)(CUvideodecoder, unsigned long long) = nullptr;
  CUresult (*get_decoder_caps)(CUVIDDECODECAPS*) = nullptr;
  CUresult (*create_parser)(CUvideoparser*, CUVIDPARSERPARAMS*) = nullptr;
  CUresult (*destroy_parser)(CUvideoparser) = nullptr;
  CUresult (*parse_video_data)(CUvideoparser, CUVIDSOURCEDATAPACKET*) = nullptr;

  bool load() {
    if (module != nullptr) {
      return true;
    }
    module = ::LoadLibraryExA("nvcuvid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      return false;
    }

    auto resolve = [this](const char* name) -> void* {
      return reinterpret_cast<void*>(::GetProcAddress(module, name));
    };

    create_decoder = reinterpret_cast<decltype(create_decoder)>(resolve("cuvidCreateDecoder"));
    destroy_decoder = reinterpret_cast<decltype(destroy_decoder)>(resolve("cuvidDestroyDecoder"));
    decode_picture = reinterpret_cast<decltype(decode_picture)>(resolve("cuvidDecodePicture"));
    map_frame64 = reinterpret_cast<decltype(map_frame64)>(resolve("cuvidMapVideoFrame64"));
    unmap_frame64 = reinterpret_cast<decltype(unmap_frame64)>(resolve("cuvidUnmapVideoFrame64"));
    get_decoder_caps =
        reinterpret_cast<decltype(get_decoder_caps)>(resolve("cuvidGetDecoderCaps"));
    create_parser =
        reinterpret_cast<decltype(create_parser)>(resolve("cuvidCreateVideoParser"));
    destroy_parser =
        reinterpret_cast<decltype(destroy_parser)>(resolve("cuvidDestroyVideoParser"));
    parse_video_data =
        reinterpret_cast<decltype(parse_video_data)>(resolve("cuvidParseVideoData"));

    return create_decoder != nullptr && create_parser != nullptr && map_frame64 != nullptr;
  }

  void unload() {
    if (module != nullptr) {
      ::FreeLibrary(module);
      module = nullptr;
    }
  }
};

cudaVideoCodec to_cuda_codec(Codec codec) noexcept {
  switch (codec) {
    case Codec::H264: return cudaVideoCodec_H264;
    case Codec::Hevc: return cudaVideoCodec_HEVC;
    case Codec::Av1: return cudaVideoCodec_AV1;
  }
  return cudaVideoCodec_HEVC;
}

}  // namespace

struct NvdecDecoder::Impl {
  CudaApi cuda;
  CuvidApi cuvid;

  ComPtr<ID3D11Device> d3d_device;
  ComPtr<ID3D11DeviceContext> d3d_context;
  ComPtr<IDXGIAdapter> adapter;

  CUcontext cu_ctx = nullptr;
  CUdevice cu_device = 0;
  bool own_context = false;
  bool d3d11_interop = false;

  CUvideoparser parser = nullptr;
  CUvideodecoder decoder = nullptr;

  Codec codec = Codec::Hevc;
  u32 width = 0;
  u32 height = 0;
  u32 pitch = 0;
  cudaVideoSurfaceFormat surface_format = cudaVideoSurfaceFormat_NV12;
  u32 decode_surfaces = 0;

  bool configured = false;
  std::string error;

  // 回调期间暂存的结果（CUVID 回调是同步的，不需要加锁）
  bool have_display = false;
  CUVIDPARSERDISPINFO pending_display{};
  u64 pending_ts = 0;

  // 解码输出纹理（D3D11 互操作路径）或 CPU 中转缓冲
  ComPtr<ID3D11Texture2D> output_texture;
  u32 texture_width = 0;
  u32 texture_height = 0;
  std::vector<u8> staging;

  // 持有 CUDA 侧注册的 D3D11 纹理，避免每帧注册/注销
  CUgraphicsResource registered_resource = nullptr;

  std::mutex mutex;

  // ---- 回调 ----
  static int CUDAAPI on_sequence(void* user, CUVIDEOFORMAT* format) {
    auto* self = static_cast<Impl*>(user);
    return self->handle_sequence(format);
  }

  static int CUDAAPI on_decode(void* user, CUVIDPICPARAMS* params) {
    auto* self = static_cast<Impl*>(user);
    return self->handle_decode(params);
  }

  static int CUDAAPI on_display(void* user, CUVIDPARSERDISPINFO* info) {
    auto* self = static_cast<Impl*>(user);
    return self->handle_display(info);
  }

  int handle_sequence(CUVIDEOFORMAT* format) {
    if (format == nullptr || format->coded_width == 0 || format->coded_height == 0) {
      return 0;
    }
    if (decoder != nullptr && format->coded_width == width && format->coded_height == height) {
      return 1;  // 分辨率没变
    }

    width = format->coded_width;
    height = format->coded_height;
    surface_format = cudaVideoSurfaceFormat_NV12;

    // 解码所需的 surface 数由码流决定；这里给足余量，NUVDEC 会按需使用。
    decode_surfaces = 32;
    if (format->min_num_decode_surfaces > decode_surfaces) {
      decode_surfaces = format->min_num_decode_surfaces;
    }

    if (!create_decoder()) {
      return 0;
    }
    DUO_INFO("NVDEC 解码器就绪: %ux%u (surfaces=%u)", width, height, decode_surfaces);
    return 1;
  }

  int handle_decode(CUVIDPICPARAMS* params) {
    if (decoder == nullptr || params == nullptr) {
      return 0;
    }
    const CUresult r = cuvid.decode_picture(decoder, params);
    if (r != CUDA_SUCCESS) {
      DUO_WARN("cuvidDecodePicture 失败: %d", static_cast<int>(r));
      return 0;
    }
    return 1;
  }

  int handle_display(CUVIDPARSERDISPINFO* info) {
    if (info == nullptr) {
      return 1;
    }
    // 只保留最新一帧：解码比呈现快，旧帧直接丢弃，避免延迟累积。
    pending_display = *info;
    have_display = true;
    return 1;
  }

  bool create_decoder() {
    if (decoder != nullptr) {
      cuvid.destroy_decoder(decoder);
      decoder = nullptr;
    }

    CUVIDDECODECREATEINFO info{};
    info.ulWidth = width;
    info.ulHeight = height;
    info.ulNumDecodeSurfaces = decode_surfaces;
    info.CodecType = to_cuda_codec(codec);
    info.ChromaFormat = cudaVideoChromaFormat_420;
    info.ulCreationFlags = cudaVideoCreate_Default;
    info.bitDepthMinus8 = 0;
    info.ulIntraDecodeOnly = 0;
    info.ulMaxWidth = width;
    info.ulMaxHeight = height;
    info.display_area.left = 0;
    info.display_area.top = 0;
    info.display_area.right = static_cast<short>(width);
    info.display_area.bottom = static_cast<short>(height);
    info.OutputFormat = surface_format;
    info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    info.ulTargetWidth = width;
    info.ulTargetHeight = height;
    info.ulNumOutputSurfaces = 2;
    info.vidLock = nullptr;

    // 注意参数顺序：句柄在前、参数在后。写反会得到 INVALID_VALUE。
    const CUresult r = cuvid.create_decoder(&decoder, &info);
    if (r != CUDA_SUCCESS) {
      DUO_ERROR("cuvidCreateDecoder 失败: %d (%ux%u)", static_cast<int>(r), width, height);
      decoder = nullptr;
      return false;
    }

    // 释放旧的输出纹理，尺寸可能变了
    output_texture.Reset();
    texture_width = 0;
    texture_height = 0;
    if (registered_resource != nullptr && cuda.graphics_unregister_resource != nullptr) {
      cuda.graphics_unregister_resource(registered_resource);
      registered_resource = nullptr;
    }
    return true;
  }

  bool ensure_context() {
    if (!cuda.load()) {
      error = "nvcuda.dll 不可用";
      return false;
    }
    if (cu_ctx != nullptr) {
      return true;
    }
    if (cuda.init(0) != CUDA_SUCCESS) {
      error = "cuInit 失败";
      return false;
    }

    int count = 0;
    if (cuda.device_get_count == nullptr || cuda.device_get_count(&count) != CUDA_SUCCESS ||
        count == 0) {
      error = "没有 CUDA 设备";
      return false;
    }
    if (cuda.device_get(&cu_device, 0) != CUDA_SUCCESS) {
      error = "cuDeviceGet 失败";
      return false;
    }

    // 优先 D3D11 互操作上下文（零拷贝）
    if (d3d11_interop && cuda.d3d11_ctx_create != nullptr && adapter != nullptr) {
      CUdevice interop_dev = 0;
      unsigned found = 0;
      if (cuda.d3d11_get_devices != nullptr &&
          cuda.d3d11_get_devices(&found, &interop_dev, 1, adapter.Get()) == CUDA_SUCCESS &&
          found == 1) {
        if (cuda.d3d11_ctx_create(&cu_ctx, 0, interop_dev, adapter.Get(), 0,
                                  d3d_device.Get()) == CUDA_SUCCESS) {
          own_context = true;
          DUO_INFO("NVDEC 使用 D3D11 互操作上下文（零拷贝）");
          return true;
        }
      }
      DUO_INFO("D3D11 互操作不可用，退回 CUDA 内存 + 上传纹理");
      d3d11_interop = false;
    }

    if (cuda.ctx_create(&cu_ctx, 0, cu_device) != CUDA_SUCCESS) {
      error = "cuCtxCreate 失败";
      return false;
    }
    own_context = true;
    return true;
  }

  void destroy_all() {
    if (parser != nullptr && cuvid.destroy_parser != nullptr) {
      cuvid.destroy_parser(parser);
      parser = nullptr;
    }
    if (decoder != nullptr) {
      cuvid.destroy_decoder(decoder);
      decoder = nullptr;
    }
    if (registered_resource != nullptr && cuda.graphics_unregister_resource != nullptr) {
      cuda.graphics_unregister_resource(registered_resource);
      registered_resource = nullptr;
    }
    output_texture.Reset();
    if (cu_ctx != nullptr && own_context && cuda.ctx_destroy != nullptr) {
      cuda.ctx_destroy(cu_ctx);
      cu_ctx = nullptr;
    }
    configured = false;
  }

  // 把 CUDA 里的解码结果搬进 D3D11 纹理
  bool upload_frame(CUdeviceptr device_ptr, unsigned int pitch, ID3D11Texture2D** out) {
    if (d3d_device == nullptr) {
      return false;
    }

    if (output_texture == nullptr || texture_width != width || texture_height != height) {
      output_texture.Reset();
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_NV12;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      if (FAILED(d3d_device->CreateTexture2D(&desc, nullptr, &output_texture))) {
        return false;
      }
      texture_width = width;
      texture_height = height;
    }

    // NV12 在显存里是 Y 平面（pitch × height）+ UV 平面（pitch × height/2）。
    const std::size_t y_size = static_cast<std::size_t>(pitch) * height;
    const std::size_t total = y_size + y_size / 2;
    if (staging.size() < total) {
      staging.resize(total);
    }

    if (cuda.memcpy_dtoh(staging.data(), device_ptr, total) != CUDA_SUCCESS) {
      return false;
    }

    // 用 UpdateSubresource 分两次写入（Y / UV），因为 D3D11 的 NV12 行距
    // 由驱动决定，不能假定等于我们的 pitch。
    D3D11_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = width;
    box.bottom = height;
    box.back = 1;
    d3d_context->UpdateSubresource(output_texture.Get(), 0, &box, staging.data(), pitch, 0);

    box.top = height;
    box.bottom = height + height / 2;
    d3d_context->UpdateSubresource(output_texture.Get(), 0, &box, staging.data() + y_size, pitch,
                                   0);

    *out = output_texture.Get();
    return true;
  }

  bool ensure_parser() {
    if (parser != nullptr) {
      return true;
    }

    CUVIDPARSERPARAMS params{};
    params.CodecType = to_cuda_codec(codec);
    params.ulMaxNumDecodeSurfaces = 32;
    params.ulClockRate = 10'000'000;
    params.ulErrorThreshold = 100;
    params.ulMaxDisplayDelay = 0;  // 低延迟：不做显示队列排队
    params.pUserData = this;
    params.pfnSequenceCallback = &Impl::on_sequence;
    params.pfnDecodePicture = &Impl::on_decode;
    params.pfnDisplayPicture = &Impl::on_display;

    const CUresult r = cuvid.create_parser(&parser, &params);
    if (r != CUDA_SUCCESS) {
      DUO_ERROR("cuvidCreateVideoParser 失败: %d", static_cast<int>(r));
      parser = nullptr;
      return false;
    }
    return true;
  }
};

NvdecDecoder::NvdecDecoder() : impl_(std::make_unique<Impl>()) {}

NvdecDecoder::~NvdecDecoder() {
  if (impl_) {
    impl_->destroy_all();
    impl_->cuda.unload();
    impl_->cuvid.unload();
  }
}

Status NvdecDecoder::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device == nullptr) {
    return Status::InvalidArgument;
  }
  impl_->d3d_device = device;
  impl_->d3d_context = context;

  ComPtr<IDXGIDevice> dxgi_device;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
    dxgi_device->GetAdapter(&impl_->adapter);
  }

  if (!impl_->cuvid.load()) {
    DUO_ERROR("nvcuvid.dll 不可用");
    return Status::NotSupported;
  }
  impl_->d3d11_interop = impl_->cuda.has_d3d11_interop();
  return Status::Ok;
}

bool NvdecDecoder::hardware_accelerated() const noexcept { return impl_->decoder != nullptr; }

Status NvdecDecoder::configure(Codec codec, ByteSpan first_access_unit) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (!impl_->cuvid.load()) {
    return Status::NotSupported;
  }

  impl_->destroy_all();
  impl_->codec = codec;

  if (!impl_->ensure_context()) {
    DUO_ERROR("NVDEC 上下文初始化失败: %s", impl_->error.c_str());
    return Status::NotSupported;
  }

  // 能力检查：不支持的编码直接放弃，不要等到第一帧才发现
  if (impl_->cuvid.get_decoder_caps != nullptr) {
    CUVIDDECODECAPS caps{};
    caps.eCodecType = to_cuda_codec(codec);
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;
    if (impl_->cuvid.get_decoder_caps(&caps) == CUDA_SUCCESS && !caps.bIsSupported) {
      DUO_ERROR("NVDEC 不支持该编码 (codec=%d)", static_cast<int>(codec));
      return Status::NotSupported;
    }
    DUO_INFO("NVDEC 能力: supported=%d 最大 %ux%u", caps.bIsSupported, caps.nMaxWidth,
             caps.nMaxHeight);
  }

  if (!impl_->ensure_parser()) {
    return Status::NotSupported;
  }

  // 用首帧里的 SPS 预先把分辨率填上，避免第一帧才触发 sequence 回调。
  const SpsInfo sps = parse_sps_dimensions(first_access_unit, codec);
  if (sps.valid) {
    impl_->width = sps.width;
    impl_->height = sps.height;
  }

  impl_->configured = true;
  return Status::Ok;
}

Status NvdecDecoder::decode(ByteSpan access_unit, u64 capture_ts_ns, DecodedFrame& out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (!impl_->configured || impl_->parser == nullptr) {
    return Status::NotInitialized;
  }
  if (access_unit.empty()) {
    return Status::InvalidArgument;
  }

  // 首帧到达前没解析出 SPS 的话，这里还能从码流里补
  if (impl_->width == 0 || impl_->height == 0) {
    const SpsInfo sps = parse_sps_dimensions(access_unit, impl_->codec);
    if (sps.valid) {
      impl_->width = sps.width;
      impl_->height = sps.height;
    }
  }

  impl_->pending_ts = capture_ts_ns;
  impl_->have_display = false;

  // 整帧作为一个 access unit 喂给解析器。Annex-B 格式，CUVID 会自己切 NAL。
  CUVIDSOURCEDATAPACKET packet{};
  packet.flags = CUVID_PKT_TIMESTAMP;
  packet.payload = access_unit.data();
  packet.payload_size = static_cast<unsigned long>(access_unit.size());
  packet.timestamp = static_cast<CUvideotimestamp>(capture_ts_ns / 100);

  const CUresult r = impl_->cuvid.parse_video_data(impl_->parser, &packet);
  if (r != CUDA_SUCCESS) {
    DUO_WARN("cuvidParseVideoData 失败: %d", static_cast<int>(r));
    return Status::IoError;
  }

  if (!impl_->have_display || impl_->decoder == nullptr) {
    return Status::WouldBlock;  // 解出了参考帧但还没有可显示的
  }

  // 把这一帧从显存映射到 CPU 可见地址
  CUdeviceptr device_ptr = 0;
  unsigned int pitch = 0;
  CUVIDPROCPARAMS proc{};
  proc.progressive_frame = 1;
  proc.top_field_first = 1;

  const CUresult mr = impl_->cuvid.map_frame64(impl_->decoder, impl_->pending_display.picture_index,
                                               &device_ptr, &pitch, &proc);
  if (mr != CUDA_SUCCESS) {
    DUO_WARN("cuvidMapVideoFrame64 失败: %d", static_cast<int>(mr));
    return Status::IoError;
  }

  ID3D11Texture2D* texture = nullptr;
  const bool ok_upload = impl_->upload_frame(device_ptr, pitch, &texture);
  impl_->cuvid.unmap_frame64(impl_->decoder, device_ptr);

  if (!ok_upload) {
    return Status::IoError;
  }

  out.texture = texture;
  out.width = impl_->width;
  out.height = impl_->height;
  out.capture_ts_ns = capture_ts_ns;
  out.key = impl_->pending_display.picture_index >= 0;
  return Status::Ok;
}

void NvdecDecoder::release(DecodedFrame& frame) {
  // 输出纹理由解码器持有并在下次解码时复用，这里只需清空调用方的指针。
  frame.texture = nullptr;
}

bool nvdec_available() noexcept {
  CuvidApi cuvid;
  if (!cuvid.load()) {
    return false;
  }
  // cuvidGetDecoderCaps 要求当前线程有 CUDA 上下文，没有就直接返回
  // CUDA_ERROR_INVALID_CONTEXT。探测时临时建一个，查完销毁。
  CudaApi cuda;
  bool available = false;
  if (cuda.load() && cuvid.get_decoder_caps != nullptr) {
    CUdevice device = 0;
    CUcontext context = nullptr;
    int count = 0;
    if (cuda.init(0) == CUDA_SUCCESS && cuda.device_get_count != nullptr &&
        cuda.device_get_count(&count) == CUDA_SUCCESS && count > 0 &&
        cuda.device_get != nullptr && cuda.device_get(&device, 0) == CUDA_SUCCESS &&
        cuda.ctx_create(&context, 0, device) == CUDA_SUCCESS) {
      CUVIDDECODECAPS caps{};
      caps.eCodecType = cudaVideoCodec_HEVC;
      caps.eChromaFormat = cudaVideoChromaFormat_420;
      caps.nBitDepthMinus8 = 0;
      if (cuvid.get_decoder_caps(&caps) == CUDA_SUCCESS && caps.bIsSupported) {
        available = true;
      }
      if (cuda.ctx_destroy != nullptr) {
        cuda.ctx_destroy(context);
      }
    }
  }
  cuda.unload();
  cuvid.unload();
  return available;
}

}  // namespace duo

#else  // !_WIN32

namespace duo {

struct NvdecDecoder::Impl {};

NvdecDecoder::NvdecDecoder() : impl_(std::make_unique<Impl>()) {}
NvdecDecoder::~NvdecDecoder() = default;
Status NvdecDecoder::initialize(ID3D11Device*, ID3D11DeviceContext*) {
  return Status::NotSupported;
}
Status NvdecDecoder::configure(Codec, ByteSpan) { return Status::NotSupported; }
Status NvdecDecoder::decode(ByteSpan, u64, DecodedFrame&) { return Status::NotSupported; }
void NvdecDecoder::release(DecodedFrame&) {}
bool NvdecDecoder::hardware_accelerated() const noexcept { return false; }
bool nvdec_available() noexcept { return false; }

}  // namespace duo

#endif
