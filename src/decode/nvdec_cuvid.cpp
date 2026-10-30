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
  CUresult (*memcpy_2d)(const CUDA_MEMCPY2D*) = nullptr;
  CUresult (*stream_synchronize)(CUstream) = nullptr;

  // D3D11 互操作（可选）
  // 注意签名：cuD3D11CtxCreateOnDevice(CUcontext*, unsigned flags,
  //                                     ID3D11Device*, CUdevice)。
  // 常见错误是照搬 OpenGL 版的 (ctx, flags, device, adapter, ...) 六参数
  // 形式——那会得到 CUDA_ERROR_UNKNOWN(999)，看起来像驱动问题。
  CUresult (*d3d11_ctx_create)(CUcontext*, unsigned, void*, CUdevice) = nullptr;
  CUresult (*d3d11_get_devices)(unsigned*, CUdevice*, unsigned, IDXGIAdapter*) = nullptr;
  CUresult (*d3d11_get_device)(CUdevice*, IDXGIAdapter*) = nullptr;
  CUresult (*graphics_d3d11_register_resource)(CUgraphicsResource*, void*, unsigned) = nullptr;
  CUresult (*graphics_map_resources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
  CUresult (*graphics_unmap_resources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
  CUresult (*graphics_get_mapped_pointer)(CUdeviceptr*, std::size_t*, CUgraphicsResource) =
      nullptr;
  // D3D11 纹理是"数组"而非线性内存，NV12 的两个平面各对应一个 array slice。
  // 必须用 SubResourceGetMappedArray 而不是 GetMappedPointer。
  CUresult (*graphics_subresource_get_array)(void**, CUgraphicsResource, unsigned, unsigned) =
      nullptr;
  CUresult (*memcpy_2d_to_array)(const CUDA_MEMCPY2D*) = nullptr;
  CUresult (*array_get_descriptor)(void*, void*) = nullptr;
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
    memcpy_2d = reinterpret_cast<decltype(memcpy_2d)>(resolve("cuMemcpy2D_v2"));
    stream_synchronize =
        reinterpret_cast<decltype(stream_synchronize)>(resolve("cuStreamSynchronize"));

    d3d11_ctx_create = reinterpret_cast<decltype(d3d11_ctx_create)>(
        resolve("cuD3D11CtxCreateOnDevice"));
    if (d3d11_ctx_create == nullptr) {
      // 老驱动只导出 v2 版，签名是 (ctx, CUdevice* pCudaDevice, flags, device)
      d3d11_ctx_create =
          reinterpret_cast<decltype(d3d11_ctx_create)>(resolve("cuD3D11CtxCreate_v2"));
    }
    d3d11_get_devices =
        reinterpret_cast<decltype(d3d11_get_devices)>(resolve("cuD3D11GetDevices_v2"));
    d3d11_get_device = reinterpret_cast<decltype(d3d11_get_device)>(resolve("cuD3D11GetDevice"));
    graphics_d3d11_register_resource = reinterpret_cast<decltype(graphics_d3d11_register_resource)>(
        resolve("cuGraphicsD3D11RegisterResource"));
    graphics_map_resources =
        reinterpret_cast<decltype(graphics_map_resources)>(resolve("cuGraphicsMapResources"));
    graphics_unmap_resources =
        reinterpret_cast<decltype(graphics_unmap_resources)>(resolve("cuGraphicsUnmapResources"));
    graphics_get_mapped_pointer = reinterpret_cast<decltype(graphics_get_mapped_pointer)>(
        resolve("cuGraphicsResourceGetMappedPointer"));
    graphics_subresource_get_array = reinterpret_cast<decltype(graphics_subresource_get_array)>(
        resolve("cuGraphicsSubResourceGetMappedArray"));
    memcpy_2d_to_array = reinterpret_cast<decltype(memcpy_2d_to_array)>(
        resolve("cuMemcpy2DToArray_v2"));
    if (memcpy_2d_to_array == nullptr) {
      memcpy_2d_to_array =
          reinterpret_cast<decltype(memcpy_2d_to_array)>(resolve("cuMemcpy2DToArray"));
    }
    array_get_descriptor =
        reinterpret_cast<decltype(array_get_descriptor)>(resolve("cuArrayGetDescriptor_v2"));
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
  bool zero_copy_enabled = false;

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
      // 用 cuD3D11GetDevice 单设备查询，不要用 cuD3D11GetDevices。
      //
      // 混合显卡笔记本上实测：GetDevices 返回 CUDA_ERROR_NO_DEVICE(100)，
      // 但 GetDevice 对同一块 adapter 返回 CUDA_SUCCESS。两个接口的
      // 过滤逻辑不同，GetDevices 会额外检查 adapter 是否被 CUDA 视为
      // "可计算设备"，而 Optimus 的显示 adapter 往往不满足。
      CUdevice interop_dev = 0;
      bool mapped = false;
      if (cuda.d3d11_get_device != nullptr) {
        const CUresult gr = cuda.d3d11_get_device(&interop_dev, adapter.Get());
        DUO_INFO("cuD3D11GetDevice -> %d dev=%d", static_cast<int>(gr),
                 static_cast<int>(interop_dev));
        mapped = (gr == CUDA_SUCCESS);
      }
      if (!mapped && cuda.d3d11_get_devices != nullptr) {
        unsigned found = 0;
        const CUresult gr = cuda.d3d11_get_devices(&found, &interop_dev, 1, adapter.Get());
        DUO_INFO("cuD3D11GetDevices -> %d count=%u", static_cast<int>(gr), found);
        mapped = (gr == CUDA_SUCCESS && found == 1);
      }

      if (mapped) {
        const CUresult cr =
            cuda.d3d11_ctx_create(&cu_ctx, 0, d3d_device.Get(), interop_dev);
        DUO_INFO("cuD3D11CtxCreateOnDevice -> %d", static_cast<int>(cr));
        if (cr == CUDA_SUCCESS) {
          own_context = true;
          DUO_INFO("NVDEC 使用 D3D11 互操作上下文（零拷贝）");
          return true;
        }
      }
      DUO_WARN("D3D11 互操作不可用，退回 CUDA 内存 + 上传纹理");
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

  // 把 CUDA 里的解码结果搬进 D3D11 纹理。
  //
  // 两条路径：
  //   * D3D11 互操作可用 —— 纹理注册成 CUDA 资源，用 cuMemcpy2D 在
  //     GPU 上直接拷。数据不出显存，CPU 完全不参与。
  //   * 互操作不可用 —— CUDA 显存 → CPU 内存 → UpdateSubresource。
  //     2560x1600 NV12 约 6 MB/帧，120fps 下就是 720 MB/s 的 PCIe + 内存
  //     带宽，是这条路径的主要开销。
  bool upload_frame(CUdeviceptr device_ptr, unsigned int pitch, ID3D11Texture2D** out) {
    if (d3d_device == nullptr) {
      return false;
    }

    if (output_texture == nullptr || texture_width != width || texture_height != height) {
      if (registered_resource != nullptr && cuda.graphics_unregister_resource != nullptr) {
        cuda.graphics_unregister_resource(registered_resource);
        registered_resource = nullptr;
      }
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

      // 注册为 CUDA 资源。只注册一次，之后每帧 map/unmap。
      //
      // 注册必须发生在我们自己的 CUDA 上下文里，且该上下文就是后面
      // map/getMappedPointer 用的那个。CUVID 会切换上下文，所以这里
      // 显式 push 一次。
      if (d3d11_interop && cuda.graphics_d3d11_register_resource != nullptr) {
        CUcontext previous = nullptr;
        if (cuda.ctx_push_current != nullptr) {
          cuda.ctx_push_current(cu_ctx);
        }
        CUgraphicsResource resource = nullptr;
        const CUresult rr = cuda.graphics_d3d11_register_resource(
            &resource, output_texture.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE);
        if (cuda.ctx_pop_current != nullptr) {
          CUcontext popped = nullptr;
          cuda.ctx_pop_current(&popped);
        }
        (void)previous;
        if (rr == CUDA_SUCCESS) {
          registered_resource = resource;
        } else {
          DUO_WARN("D3D11 纹理注册为 CUDA 资源失败: %d", static_cast<int>(rr));
          d3d11_interop = false;
        }
      }
    }

    if (registered_resource != nullptr) {
      if (copy_on_gpu(device_ptr, pitch)) {
        *out = output_texture.Get();
        return true;
      }
      // GPU 拷贝失败就降级，避免每帧都试一遍
      DUO_WARN("GPU 侧拷贝失败，降级为 CPU 拷贝");
      cuda.graphics_unregister_resource(registered_resource);
      registered_resource = nullptr;
      d3d11_interop = false;
    }

    return copy_via_cpu(device_ptr, pitch, out);
  }

  // 零拷贝路径（实验性，默认不启用）。
  //
  // 状态：已打通到「互操作上下文创建成功 + Y 平面 2D 拷贝成功」，
  // 但 UV 平面拿不到第二个 array slice —— D3D11 的 NV12 纹理注册成
  // CUDA 资源后只暴露一个 array，其高度等于画面高度，UV 没有独立
  // slice 可写。cuMemcpy2D 的 dstY 超出高度即返回 INVALID_VALUE。
  //
  // 要让这条路径真正可用，需要改用 CUDA 的 external memory 接口
  // （cuImportExternalMemory + cuExternalMemoryGetMappedMipmappedArray），
  // 按 plane 分别 import —— 那是另一套 API，工作量不小。
  //
  // 当前默认走 copy_via_cpu，实测 2560x1600 端到端 0.3ms，够用。
  // 保留这个函数是为了让 enable_zero_copy 打开时能快速验证。
  bool copy_on_gpu(CUdeviceptr source, unsigned int source_pitch) {
    if (cuda.graphics_map_resources == nullptr ||
        cuda.graphics_subresource_get_array == nullptr || cuda.memcpy_2d == nullptr ||
        cuda.graphics_unmap_resources == nullptr) {
      return false;
    }

    CUcontext previous = nullptr;
    if (cuda.ctx_push_current != nullptr &&
        cuda.ctx_push_current(cu_ctx) != CUDA_SUCCESS) {
      return false;
    }

    CUstream stream = nullptr;
    if (cuda.graphics_map_resources(1, &registered_resource, stream) != CUDA_SUCCESS) {
      if (cuda.ctx_pop_current != nullptr) {
        CUcontext popped = nullptr;
        cuda.ctx_pop_current(&popped);
      }
      return false;
    }

    bool ok = false;
    void* luma_raw = nullptr;
    if (cuda.graphics_subresource_get_array(&luma_raw, registered_resource, 0, 0) ==
        CUDA_SUCCESS) {
      auto luma_array = reinterpret_cast<CUarray>(luma_raw);
      CUDA_MEMCPY2D y{};
      y.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      y.srcDevice = source;
      y.srcPitch = source_pitch;
      y.dstMemoryType = CU_MEMORYTYPE_ARRAY;
      y.dstArray = luma_array;
      y.WidthInBytes = width;
      y.Height = height;
      ok = (cuda.memcpy_2d(&y) == CUDA_SUCCESS);
    }

    cuda.graphics_unmap_resources(1, &registered_resource, stream);
    if (cuda.ctx_pop_current != nullptr) {
      CUcontext popped = nullptr;
      cuda.ctx_pop_current(&popped);
    }
    (void)previous;
    return ok;
  }

  // D3D11 NV12 纹理的行距对齐。驱动通常按 256 字节对齐。
  [[nodiscard]] u32 aligned_width() const noexcept {
    return ((width + 255) / 256) * 256;
  }

  // 兜底路径：显存 → CPU → 纹理。
  bool copy_via_cpu(CUdeviceptr device_ptr, unsigned int pitch, ID3D11Texture2D** out) {
    const std::size_t y_size = static_cast<std::size_t>(pitch) * height;
    const std::size_t total = y_size + y_size / 2;
    if (staging.size() < total) {
      staging.resize(total);
    }

    if (cuda.memcpy_dtoh(staging.data(), device_ptr, total) != CUDA_SUCCESS) {
      return false;
    }

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
  // 这里必须先 load，否则 cuda.* 全是 nullptr，has_d3d11_interop()
  // 会永远返回 false —— 表现为「互操作不可用」，但实际只是没初始化。
  if (!impl_->cuda.load()) {
    DUO_WARN("nvcuda.dll 不可用，NVDEC 无法工作");
    return Status::NotSupported;
  }
  // 互操作上下文能建起来，但 NV12 的 UV 平面写不进去（见 copy_on_gpu
  // 注释）。所以只有显式开启零拷贝时才走互操作上下文。
  const bool interop_capable = impl_->cuda.has_d3d11_interop();
  impl_->d3d11_interop = interop_capable && impl_->zero_copy_enabled;
  DUO_INFO("D3D11 互操作: %s（零拷贝%s）", interop_capable ? "可用" : "不可用",
           impl_->zero_copy_enabled ? "已启用" : "默认关闭，走 CPU 拷贝");
  return Status::Ok;
}

bool NvdecDecoder::hardware_accelerated() const noexcept { return impl_->decoder != nullptr; }

void NvdecDecoder::set_zero_copy(bool enabled) noexcept {
  impl_->zero_copy_enabled = enabled;
}

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

  // 不在这里 push：copy_on_gpu / copy_via_cpu 内部会自己做完整的
  // push/pop。多压一层会让上下文栈失衡，后续 getMappedPointer 报 201。

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
