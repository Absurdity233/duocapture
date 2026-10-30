#include "duocapture/capture/color_converter.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3dcompiler.h>

#include <cstring>

#include "duocapture/common/log.h"

namespace duo {
namespace {

// 两趟共用同一个 shader，靠 pass 常量区分：
//   pass 0 -> 写 Y 平面（每像素，R8）
//   pass 1 -> 写 UV 平面（每 2x2 块，R8G8）
//
// 色度用 2x2 盒式平均而不是点采样。细线条和高对比边缘上差距很明显，
// 代价几乎为零。
constexpr const char* kShaderSource = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint mode;
};

Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

// BT.709 全范围 RGB -> limited range YCbCr
float3 rgb_to_ycbcr(float3 rgb)
{
    float y  =  0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b;
    float cb = (rgb.b - y) / 1.8556f;
    float cr = (rgb.r - y) / 1.5748f;
    // limited range：Y 映射到 [16,235]，色差映射到 [-0.5,0.5] 再压到 16-240
    return float3((y * 219.0f + 16.0f) / 255.0f,
                  (cb * 224.0f + 128.0f) / 255.0f,
                  (cr * 224.0f + 128.0f) / 255.0f);
}

float3 load_rgb(int2 p)
{
    p = clamp(p, int2(0, 0), int2(width - 1, height - 1));
    return src.Load(int3(p, 0)).rgb;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (mode == 0)
    {
        // 亮度：一次 dispatch 处理一行，64 线程 x 8 像素 = 512 像素
        int y = int(gid.x);
        if (y >= int(height)) return;
        int base_x = int(gid.y * 64 + lane) * 8;
        for (int i = 0; i < 8; ++i)
        {
            int x = base_x + i;
            if (x >= int(width)) break;
            float3 ycc = rgb_to_ycbcr(load_rgb(int2(x, y)));
            dst[int2(x, y)] = float4(ycc.x, 0, 0, 1);
        }
    }
    else
    {
        // 色度：每个 2x2 块一像素，所以行列都减半
        int cy = int(gid.x);
        int chroma_height = int(height) / 2;
        if (cy >= chroma_height) return;
        int base_cx = int(gid.y * 64 + lane) * 8;
        for (int i = 0; i < 8; ++i)
        {
            int cx = base_cx + i;
            if (cx >= int(width) / 2) break;
            int px = cx * 2;
            int py = cy * 2;
            float3 c0 = load_rgb(int2(px,     py    ));
            float3 c1 = load_rgb(int2(px + 1, py    ));
            float3 c2 = load_rgb(int2(px,     py + 1));
            float3 c3 = load_rgb(int2(px + 1, py + 1));
            float3 ycc = rgb_to_ycbcr((c0 + c1 + c2 + c3) * 0.25f);
            // R8G8_UNORM 视图：x=R=Cb, y=G=Cr
            dst[int2(cx, cy)] = float4(ycc.y, ycc.z, 0, 1);
        }
    }
}
)";

}  // namespace

Status ColorConverter::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device == nullptr || context == nullptr) {
    return Status::InvalidArgument;
  }
  device->AddRef();
  context->AddRef();
  device_ = device;
  context_ = context;
  return ensure_shader();
}

void ColorConverter::shutdown() {
  auto release = [](auto*& ptr) {
    if (ptr != nullptr) {
      ptr->Release();
      ptr = nullptr;
    }
  };
  release(chroma_uav_);
  release(luma_uav_);
  release(output_);
  release(input_srv_);
  release(input_texture_);
  release(params_);
  release(shader_);
  release(context_);
  release(device_);
  width_ = 0;
  height_ = 0;
}

Status ColorConverter::ensure_shader() {
  if (shader_ != nullptr) {
    return Status::Ok;
  }

  ID3DBlob* error_blob = nullptr;
  ID3DBlob* shader_blob = nullptr;
  const HRESULT hr = ::D3DCompile(kShaderSource, std::strlen(kShaderSource), "nv12_convert",
                                  nullptr, nullptr, "main", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader_blob, &error_blob);
  if (FAILED(hr)) {
    DUO_ERROR("NV12 转换 shader 编译失败: %s",
              error_blob != nullptr ? static_cast<const char*>(error_blob->GetBufferPointer())
                                    : "unknown");
    if (error_blob != nullptr) error_blob->Release();
    return Status::NotSupported;
  }

  const HRESULT create_hr =
      device_->CreateComputeShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(),
                                   nullptr, &shader_);
  shader_blob->Release();
  if (FAILED(create_hr)) {
    DUO_ERROR("创建 compute shader 失败: 0x%08X", static_cast<unsigned>(create_hr));
    return Status::NotSupported;
  }

  D3D11_BUFFER_DESC buffer_desc{};
  buffer_desc.ByteWidth = 16;  // 常量缓冲必须 16 字节对齐
  buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  const HRESULT buf_hr = device_->CreateBuffer(&buffer_desc, nullptr, &params_);
  if (FAILED(buf_hr)) {
    DUO_ERROR("创建常量缓冲失败: 0x%08X", static_cast<unsigned>(buf_hr));
    return Status::NotSupported;
  }
  DUO_INFO("NV12 转换器就绪");
  return Status::Ok;
}

Status ColorConverter::ensure_output(u32 width, u32 height) {
  if (output_ != nullptr && width_ == width && height_ == height) {
    return Status::Ok;
  }

  if (chroma_uav_ != nullptr) {
    chroma_uav_->Release();
    chroma_uav_ = nullptr;
  }
  if (luma_uav_ != nullptr) {
    luma_uav_->Release();
    luma_uav_ = nullptr;
  }
  if (output_ != nullptr) {
    output_->Release();
    output_ = nullptr;
  }

  if ((width & 1u) != 0u || (height & 1u) != 0u) {
    DUO_ERROR("NV12 要求偶数尺寸，收到 %ux%u", width, height);
    return Status::InvalidArgument;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height + height / 2;  // Y 平面 + 半高 UV 平面
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

  const HRESULT tex_hr = device_->CreateTexture2D(&desc, nullptr, &output_);
  if (FAILED(tex_hr)) {
    DUO_ERROR("创建 NV12 输出纹理失败 %ux%u (0x%08X)", width, height,
              static_cast<unsigned>(tex_hr));
    return Status::NotSupported;
  }

  // 平面视图依赖驱动支持。NVIDIA / Intel / AMD 的 D3D11 驱动都实现了
  // NV12 的平面 UAV，但这是可选项，失败时要给出明确原因。
  D3D11_UNORDERED_ACCESS_VIEW_DESC luma_desc{};
  luma_desc.Format = DXGI_FORMAT_R8_UNORM;
  luma_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  luma_desc.Texture2D.MipSlice = 0;
  const HRESULT luma_hr = device_->CreateUnorderedAccessView(output_, &luma_desc, &luma_uav_);

  D3D11_UNORDERED_ACCESS_VIEW_DESC chroma_desc{};
  chroma_desc.Format = DXGI_FORMAT_R8G8_UNORM;
  chroma_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  chroma_desc.Texture2D.MipSlice = 0;
  const HRESULT chroma_hr =
      device_->CreateUnorderedAccessView(output_, &chroma_desc, &chroma_uav_);

  if (FAILED(luma_hr) || FAILED(chroma_hr)) {
    DUO_ERROR("创建 NV12 平面 UAV 失败（luma=0x%08X chroma=0x%08X）",
              static_cast<unsigned>(luma_hr), static_cast<unsigned>(chroma_hr));
    return Status::NotSupported;
  }

  width_ = width;
  height_ = height;
  return Status::Ok;
}

Status ColorConverter::ensure_input_srv(ID3D11Texture2D* source) {
  if (input_texture_ == source && input_srv_ != nullptr) {
    return Status::Ok;
  }

  if (input_srv_ != nullptr) {
    input_srv_->Release();
    input_srv_ = nullptr;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
  srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Texture2D.MipLevels = 1;

  if (FAILED(device_->CreateShaderResourceView(source, &srv_desc, &input_srv_))) {
    return Status::NotSupported;
  }
  input_texture_ = source;
  return Status::Ok;
}

void ColorConverter::dispatch(ID3D11UnorderedAccessView* uav, u32 mode, u32 rows,
                              u32 columns) const {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(params_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return;
  }
  auto* data = static_cast<u32*>(mapped.pData);
  data[0] = width_;
  data[1] = height_;
  data[2] = mode;
  context_->Unmap(params_, 0);

  ID3D11ShaderResourceView* srvs[] = {input_srv_};
  ID3D11UnorderedAccessView* uavs[] = {uav};
  ID3D11Buffer* buffers[] = {params_};

  context_->CSSetShader(shader_, nullptr, 0);
  context_->CSSetShaderResources(0, 1, srvs);
  context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
  context_->CSSetConstantBuffers(0, 1, buffers);
  context_->Dispatch(rows, columns, 1);
}

Status ColorConverter::convert(ID3D11Texture2D* source, ID3D11Texture2D** nv12_out) {
  if (source == nullptr || nv12_out == nullptr) {
    return Status::InvalidArgument;
  }

  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);

  Status status = ensure_output(desc.Width, desc.Height);
  if (!ok(status)) return status;

  status = ensure_input_srv(source);
  if (!ok(status)) return status;

  // 每行一次 group（Y），每 group 64 线程 x 8 像素
  const u32 luma_columns = (width_ + 8 * 64 - 1) / (8 * 64);
  dispatch(luma_uav_, 0, height_, luma_columns);

  const u32 chroma_rows = height_ / 2;
  const u32 chroma_columns = (width_ / 2 + 8 * 64 - 1) / (8 * 64);
  dispatch(chroma_uav_, 1, chroma_rows, chroma_columns);

  ID3D11ShaderResourceView* null_srv[] = {nullptr};
  ID3D11UnorderedAccessView* null_uav[] = {nullptr};
  context_->CSSetShaderResources(0, 1, null_srv);
  context_->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
  context_->CSSetShader(nullptr, nullptr, 0);

  *nv12_out = output_;
  return Status::Ok;
}

}  // namespace duo

#else

namespace duo {

// 非 Windows 平台的空实现已在头文件中内联，这里不再重复定义。

}  // namespace duo

#endif
