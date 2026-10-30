#include "duocapture/present/box_overlay.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "duocapture/common/log.h"

using Microsoft::WRL::ComPtr;

namespace duo {
namespace {

// 按类别选颜色。0 = 让宿主自动配色（插件没指定时）。
u32 class_color(int class_id) {
  static const u32 kPalette[] = {
      0xFF00E040u,  // 绿
      0xFFFF4040u,  // 红
      0xFF40A0FFu,  // 蓝
      0xFFFFD040u,  // 黄
      0xFFC040FFu,  // 紫
      0xFF40FFFFu,  // 青
      0xFFFF8040u,  // 橙
      0xFFFF40C0u,  // 粉
  };
  constexpr int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
  const int idx = class_id < 0 ? 0 : class_id % n;
  return kPalette[idx];
}

}  // namespace

struct BoxOverlay::Impl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;

  // 离屏画布（BGRA，自上而下）
  HDC memory_dc = nullptr;
  HBITMAP bitmap = nullptr;
  void* bits = nullptr;
  int canvas_width = 0;
  int canvas_height = 0;

  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  u32 texture_width = 0;
  u32 texture_height = 0;

  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11DepthStencilState> depth;

  ~Impl() { release_canvas(); }

  void release_canvas() {
    if (bitmap != nullptr) {
      ::DeleteObject(bitmap);
      bitmap = nullptr;
    }
    if (memory_dc != nullptr) {
      ::DeleteDC(memory_dc);
      memory_dc = nullptr;
    }
    bits = nullptr;
    canvas_width = 0;
    canvas_height = 0;
  }

  bool ensure_canvas(int width, int height) {
    if (memory_dc != nullptr && canvas_width == width && canvas_height == height) {
      return true;
    }
    release_canvas();

    HDC screen = ::GetDC(nullptr);
    memory_dc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (memory_dc == nullptr) {
      return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // 负数 = 自上而下，与 D3D 纹理一致
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* raw = nullptr;
    bitmap = ::CreateDIBSection(memory_dc, &bmi, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (bitmap == nullptr || raw == nullptr) {
      release_canvas();
      return false;
    }
    bits = raw;
    ::SelectObject(memory_dc, bitmap);
    canvas_width = width;
    canvas_height = height;
    return true;
  }

  // 在画布上光栅化若干矩形边框。坐标已经换算到画布尺寸。
  void render_boxes(const DuoOverlayBox* boxes, int count, float sx, float sy) {
    if (memory_dc == nullptr || bits == nullptr) {
      return;
    }
    std::memset(bits, 0, static_cast<std::size_t>(canvas_width) * canvas_height * 4);

    const int thickness = std::max(2, canvas_width / 480);  // 跟着分辨率走
    for (int i = 0; i < count; ++i) {
      const DuoOverlayBox& b = boxes[i];
      const int x1 = static_cast<int>(std::lround(b.x1 * sx));
      const int y1 = static_cast<int>(std::lround(b.y1 * sy));
      const int x2 = static_cast<int>(std::lround(b.x2 * sx));
      const int y2 = static_cast<int>(std::lround(b.y2 * sy));
      if (x2 <= x1 || y2 <= y1) {
        continue;
      }

      const u32 argb = b.color != 0 ? b.color : class_color(b.class_id);
      // GDI 用的是 COLORREF(0x00BBGGRR)，不是 ARGB
      const COLORREF color =
          RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);

      HBRUSH brush = ::CreateSolidBrush(color);
      // 画四条边而不是 4 个矩形，避免半透明叠加后边框颜色不均
      RECT top{x1, y1, x2, y1 + thickness};
      RECT bottom{x1, y2 - thickness, x2, y2};
      RECT left{x1, y1, x1 + thickness, y2};
      RECT right{x2 - thickness, y1, x2, y2};
      ::FillRect(memory_dc, &top, brush);
      ::FillRect(memory_dc, &bottom, brush);
      ::FillRect(memory_dc, &left, brush);
      ::FillRect(memory_dc, &right, brush);
      ::DeleteObject(brush);
    }

    // GDI 写出来的是 0x00BBGGRR（无 alpha）。按「非黑即不透明」补 alpha，
    // 与 TelemetryOverlay 的做法一致。
    auto* pixels = static_cast<u32*>(bits);
    const std::size_t total = static_cast<std::size_t>(canvas_width) * canvas_height;
    for (std::size_t i = 0; i < total; ++i) {
      const u32 rgb = pixels[i] & 0x00FFFFFFu;
      pixels[i] = (rgb == 0) ? 0u : (rgb | 0xFF000000u);
    }
  }

  bool ensure_texture(u32 width, u32 height) {
    if (texture != nullptr && texture_width == width && texture_height == height) {
      return true;
    }
    texture.Reset();
    srv.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
      return false;
    }
    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
      return false;
    }
    texture_width = width;
    texture_height = height;
    return true;
  }

  bool ensure_pipeline() {
    if (vs != nullptr && ps != nullptr) {
      return true;
    }

    static const char* kVs = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 quad[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    float2 p = quad[id];
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = p;
    return o;
}
)";
    static const char* kPs = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(samp, uv);
}
)";

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> error;
    if (FAILED(::D3DCompile(kVs, std::strlen(kVs), "box_vs", nullptr, nullptr, "main", "vs_5_0", 0,
                            0, &vs_blob, &error))) {
      return false;
    }
    if (FAILED(::D3DCompile(kPs, std::strlen(kPs), "box_ps", nullptr, nullptr, "main", "ps_5_0", 0,
                            0, &ps_blob, &error))) {
      return false;
    }
    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                          nullptr, &vs))) {
      return false;
    }
    if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                         nullptr, &ps))) {
      return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sd, &sampler))) {
      return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &blend))) {
      return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&rd, &raster))) {
      return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dd, &depth))) {
      return false;
    }
    return true;
  }
};

BoxOverlay::BoxOverlay() = default;
BoxOverlay::~BoxOverlay() = default;

Status BoxOverlay::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device == nullptr || context == nullptr) {
    return Status::InvalidArgument;
  }
  impl_ = std::make_unique<Impl>();
  impl_->device = device;
  impl_->context = context;
  return Status::Ok;
}

void BoxOverlay::shutdown() { impl_.reset(); }

Status BoxOverlay::draw(ID3D11Texture2D* target, u32 target_width, u32 target_height,
                        const DuoOverlayBox* boxes, int count, u32 source_width,
                        u32 source_height) {
  if (!enabled_ || impl_ == nullptr || target == nullptr || boxes == nullptr || count <= 0) {
    return Status::Ok;
  }
  if (target_width == 0 || target_height == 0 || source_width == 0 || source_height == 0) {
    return Status::InvalidArgument;
  }
  if (!impl_->ensure_canvas(static_cast<int>(target_width), static_cast<int>(target_height))) {
    return Status::NotSupported;
  }

  // 源帧 → 目标表面是整幅拉伸（视频处理器默认行为），所以线性缩放即可。
  const float sx = static_cast<float>(target_width) / static_cast<float>(source_width);
  const float sy = static_cast<float>(target_height) / static_cast<float>(source_height);
  impl_->render_boxes(boxes, count, sx, sy);

  if (!impl_->ensure_texture(target_width, target_height) || !impl_->ensure_pipeline()) {
    return Status::NotSupported;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(impl_->context->Map(impl_->texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return Status::IoError;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(target_width) * 4;
  auto* dst = static_cast<u8*>(mapped.pData);
  const auto* src = static_cast<const u8*>(impl_->bits);
  const int rows = static_cast<int>(target_height);
  for (int y = 0; y < rows; ++y) {
    std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                src + static_cast<std::size_t>(y) * row_bytes, row_bytes);
  }
  impl_->context->Unmap(impl_->texture.Get(), 0);

  ComPtr<ID3D11RenderTargetView> rtv;
  if (FAILED(impl_->device->CreateRenderTargetView(target, nullptr, &rtv))) {
    return Status::IoError;
  }
  impl_->context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

  D3D11_VIEWPORT vp{};
  vp.Width = static_cast<float>(target_width);
  vp.Height = static_cast<float>(target_height);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  impl_->context->RSSetViewports(1, &vp);
  impl_->context->RSSetState(impl_->raster.Get());

  const float blend_factor[4] = {0, 0, 0, 0};
  impl_->context->OMSetBlendState(impl_->blend.Get(), blend_factor, 0xFFFFFFFF);
  impl_->context->OMSetDepthStencilState(impl_->depth.Get(), 0);

  impl_->context->IASetInputLayout(nullptr);
  impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
  impl_->context->PSSetShader(impl_->ps.Get(), nullptr, 0);
  impl_->context->PSSetShaderResources(0, 1, impl_->srv.GetAddressOf());
  impl_->context->PSSetSamplers(0, 1, impl_->sampler.GetAddressOf());
  impl_->context->Draw(4, 0);

  // 解绑，避免影响后续视频处理器
  ID3D11RenderTargetView* null_rtv = nullptr;
  impl_->context->OMSetRenderTargets(1, &null_rtv, nullptr);
  ID3D11ShaderResourceView* null_srv = nullptr;
  impl_->context->PSSetShaderResources(0, 1, &null_srv);
  impl_->context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  return Status::Ok;
}

}  // namespace duo

#else

namespace duo {

BoxOverlay::BoxOverlay() = default;
BoxOverlay::~BoxOverlay() = default;

}  // namespace duo

#endif
