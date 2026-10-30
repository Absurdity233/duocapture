#include "duocapture/present/telemetry_overlay.h"

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

#include <cstring>
#include <memory>
#include <vector>

#include "duocapture/common/log.h"

using Microsoft::WRL::ComPtr;

namespace duo {

struct TelemetryOverlay::Impl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;

  // 离屏画布：32 位 BGRA，GDI 可以直接画
  HDC memory_dc = nullptr;
  HBITMAP bitmap = nullptr;
  void* bits = nullptr;
  int canvas_width = 0;
  int canvas_height = 0;

  // 上传到 GPU 的纹理
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  u32 texture_width = 0;
  u32 texture_height = 0;

  // 绘制用：一个简单的全屏三角形着色器，把叠加纹理 alpha 混合上去
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11DepthStencilState> depth;

  std::vector<OverlayLine> lines;

  bool ensure_canvas(int width, int height) {
    if (memory_dc != nullptr && canvas_width == width && canvas_height == height) {
      return true;
    }

    if (bitmap != nullptr) {
      ::DeleteObject(bitmap);
      bitmap = nullptr;
    }
    if (memory_dc != nullptr) {
      ::DeleteDC(memory_dc);
      memory_dc = nullptr;
    }

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
      return false;
    }
    bits = raw;
    ::SelectObject(memory_dc, bitmap);

    canvas_width = width;
    canvas_height = height;
    return true;
  }

  void render_text() {
    if (memory_dc == nullptr || bits == nullptr) {
      return;
    }

    // 先清成全透明，否则上一帧的文字会残留
    std::memset(bits, 0, static_cast<std::size_t>(canvas_width) * canvas_height * 4);

    const int line_height = 20;
    const int padding = 10;
    const int box_width = 380;
    const int box_height = padding * 2 + line_height * static_cast<int>(lines.size()) + 8;

    // 半透明黑底，让白字在任何画面上都看得清
    HBRUSH back = ::CreateSolidBrush(RGB(0, 0, 0));
    RECT box{10, 10, 10 + box_width, 10 + box_height};
    ::FillRect(memory_dc, &box, back);
    ::DeleteObject(back);

    ::SetBkMode(memory_dc, TRANSPARENT);
    ::SetTextColor(memory_dc, RGB(255, 255, 255));

    HFONT font = ::CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FF_DONTCARE, "Consolas");
    HGDIOBJ old_font = ::SelectObject(memory_dc, font);

    int y = 10 + padding;
    for (const OverlayLine& line : lines) {
      const std::string text = line.label + ": " + line.value;
      ::TextOutA(memory_dc, 10 + padding, y, text.c_str(), static_cast<int>(text.size()));
      y += line_height;
    }

    ::SelectObject(memory_dc, old_font);
    ::DeleteObject(font);

    // GDI 写的是 0x00BBGGRR（无 alpha），把它补成不透明 alpha。
    // 文字像素 alpha=255，背景像素 alpha=0（保持透明）。
    auto* pixels = static_cast<u32*>(bits);
    const std::size_t count = static_cast<std::size_t>(canvas_width) * canvas_height;
    for (std::size_t i = 0; i < count; ++i) {
      const u32 rgb = pixels[i] & 0x00FFFFFFu;
      // 非黑即文字（或半透明底）。给非纯黑像素补 alpha。
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

    // 顶点着色器用 SV_VertexID 生成全屏四边形，不需要顶点缓冲
    static const char* kVs = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    // 只需要左上角一小块区域，按 UV 映射过去
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
    if (FAILED(::D3DCompile(kVs, std::strlen(kVs), "overlay_vs", nullptr, nullptr, "main",
                            "vs_5_0", 0, 0, &vs_blob, &error))) {
      return false;
    }
    if (FAILED(::D3DCompile(kPs, std::strlen(kPs), "overlay_ps", nullptr, nullptr, "main",
                            "ps_5_0", 0, 0, &ps_blob, &error))) {
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
    rd.DepthClipEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, &raster))) {
      return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateDepthStencilState(&dd, &depth))) {
      return false;
    }
    return true;
  }
};

TelemetryOverlay::TelemetryOverlay() : impl_(std::make_unique<Impl>()) {}

TelemetryOverlay::~TelemetryOverlay() { shutdown(); }

Status TelemetryOverlay::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (device == nullptr || context == nullptr) {
    return Status::InvalidArgument;
  }
  impl_->device = device;
  impl_->context = context;
  if (!impl_->ensure_pipeline()) {
    DUO_WARN("遥测 overlay 管线创建失败（将不显示统计）");
    return Status::NotSupported;
  }
  return Status::Ok;
}

void TelemetryOverlay::shutdown() {
  if (!impl_) {
    return;
  }
  if (impl_->bitmap != nullptr) {
    ::DeleteObject(impl_->bitmap);
    impl_->bitmap = nullptr;
  }
  if (impl_->memory_dc != nullptr) {
    ::DeleteDC(impl_->memory_dc);
    impl_->memory_dc = nullptr;
  }
  impl_->bits = nullptr;
  impl_->canvas_width = 0;
  impl_->canvas_height = 0;
  impl_->srv.Reset();
  impl_->texture.Reset();
  impl_->blend.Reset();
  impl_->raster.Reset();
  impl_->depth.Reset();
  impl_->sampler.Reset();
  impl_->ps.Reset();
  impl_->vs.Reset();
  impl_->context.Reset();
  impl_->device.Reset();
}

void TelemetryOverlay::set_lines(const std::vector<OverlayLine>& lines) {
  impl_->lines = lines;
}

Status TelemetryOverlay::draw(ID3D11Texture2D* target, u32 target_width, u32 target_height) {
  if (!enabled_ || impl_->device == nullptr || target == nullptr) {
    return Status::Ok;
  }
  if (impl_->lines.empty()) {
    return Status::Ok;
  }

  // 画布只需要覆盖左上角一小块，640x480 够放十行统计
  const int canvas_w = 420;
  const int canvas_h = 40 + static_cast<int>(impl_->lines.size()) * 20;
  if (!impl_->ensure_canvas(canvas_w, canvas_h)) {
    return Status::NotSupported;
  }
  impl_->render_text();

  if (!impl_->ensure_texture(static_cast<u32>(canvas_w), static_cast<u32>(canvas_h))) {
    return Status::NotSupported;
  }

  // 上传画布
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(impl_->context->Map(impl_->texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return Status::IoError;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(canvas_w) * 4;
  auto* dst = static_cast<u8*>(mapped.pData);
  const auto* src = static_cast<const u8*>(impl_->bits);
  for (int y = 0; y < canvas_h; ++y) {
    std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                src + static_cast<std::size_t>(y) * row_bytes, row_bytes);
  }
  impl_->context->Unmap(impl_->texture.Get(), 0);

  // 用后备缓冲做渲染目标
  ComPtr<ID3D11RenderTargetView> rtv;
  D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
  rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  if (FAILED(impl_->device->CreateRenderTargetView(target, &rtv_desc, &rtv))) {
    return Status::IoError;
  }

  impl_->context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  const float clear[4] = {0, 0, 0, 0};
  (void)clear;

  D3D11_VIEWPORT vp{};
  vp.Width = static_cast<float>(canvas_w);
  vp.Height = static_cast<float>(canvas_h);
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

  (void)target_width;
  (void)target_height;
  return Status::Ok;
}

}  // namespace duo

#else

namespace duo {

TelemetryOverlay::TelemetryOverlay() = default;
TelemetryOverlay::~TelemetryOverlay() = default;

}  // namespace duo

#endif
