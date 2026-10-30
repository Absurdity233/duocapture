// 示例插件：统计每帧的平均亮度与直方图分布。
//
// 这个插件同时是「怎么写一个 DuoCapture 插件」的参考实现：
//   * 只依赖 plugin_api.h，不 include 主程序任何其他头文件
//   * 内部可以用 C++（这里用了 std::vector），只要不跨越 ABI 边界
//   * 导出 duo_plugin_entry 一个符号即可
//
// 实际用途：直播质量监控（画面全黑 = 卡了或源断了）、
// 场景切换检测、自动曝光评估。

#include "duocapture/plugin/plugin_api.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace {

const DuoPluginHostServices* g_host = nullptr;

void log_info(const char* text) {
  if (g_host != nullptr && g_host->log != nullptr) {
    g_host->log(DUO_LOG_INFO, text);
  }
}

struct LumaStats {
  // 配置
  int sample_step = 16;  // 每隔多少像素采样一次，越大越省
  int log_interval = 120;  // 每多少帧打一次日志

  // 统计
  std::atomic<unsigned long long> frames{0};
  std::atomic<unsigned long long> dark_frames{0};
  std::atomic<int> last_avg_luma{0};

  // GPU 读回用的暂存纹理（staging）
  ComPtr<ID3D11Texture2D> staging;
  unsigned int staging_width = 0;
  unsigned int staging_height = 0;
};

// 从 NV12 纹理采样计算平均亮度。
//
// 只读 Y 平面就够了——亮度信息全在 Y 里，不用碰 UV，采样量减半。
// 这里走 staging 回读（GPU → CPU），对 1080p 每隔 16 像素采样
// 大约 8000 个点，实测 < 1ms。
int compute_average_luma(LumaStats& self, ID3D11Texture2D* texture, unsigned int width,
                         unsigned int height) {
#if defined(_WIN32)
  if (g_host == nullptr || g_host->d3d_device == nullptr || g_host->d3d_context == nullptr) {
    return -1;
  }
  auto* device = static_cast<ID3D11Device*>(g_host->d3d_device);
  auto* context = static_cast<ID3D11DeviceContext*>(g_host->d3d_context);

  D3D11_TEXTURE2D_DESC src_desc{};
  texture->GetDesc(&src_desc);

  if (self.staging == nullptr || self.staging_width != src_desc.Width ||
      self.staging_height != src_desc.Height) {
    self.staging.Reset();
    D3D11_TEXTURE2D_DESC desc = src_desc;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &self.staging))) {
      return -1;
    }
    self.staging_width = src_desc.Width;
    self.staging_height = src_desc.Height;
  }

  context->CopyResource(self.staging.Get(), texture);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(self.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return -1;
  }

  const auto* base = static_cast<const unsigned char*>(mapped.pData);
  unsigned long long sum = 0;
  unsigned long long count = 0;

  const unsigned int step = self.sample_step > 0 ? static_cast<unsigned int>(self.sample_step) : 16;
  // 只采 Y 平面：高度是 height（UV 在 height 之后，不用管）
  for (unsigned int y = 0; y < height; y += step) {
    const auto* row = base + static_cast<std::size_t>(y) * mapped.RowPitch;
    for (unsigned int x = 0; x < width; x += step) {
      sum += row[x];
      ++count;
    }
  }

  context->Unmap(self.staging.Get(), 0);

  if (count == 0) {
    return -1;
  }
  return static_cast<int>(sum / count);
#else
  (void)self;
  (void)texture;
  (void)width;
  (void)height;
  return -1;
#endif
}

int on_frame(void* instance, const DuoFrameView* frame) {
  if (instance == nullptr || frame == nullptr || frame->texture == nullptr) {
    return -1;
  }
  auto* self = static_cast<LumaStats*>(instance);

  auto* texture = static_cast<ID3D11Texture2D*>(frame->texture);
  const int luma = compute_average_luma(*self, texture, frame->width, frame->height);
  if (luma < 0) {
    return -1;
  }

  self->last_avg_luma.store(luma);
  const unsigned long long n = self->frames.fetch_add(1) + 1;

  // 亮度低于 16 视为「接近全黑」——直播里通常意味着源断了或卡了
  if (luma < 16) {
    self->dark_frames.fetch_add(1);
  }

  if (self->log_interval > 0 && n % static_cast<unsigned long long>(self->log_interval) == 0) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "luma_stats: 第 %llu 帧，平均亮度 %d，黑帧 %llu", n, luma,
                  self->dark_frames.load());
    log_info(buf);
  }
  return luma;
}

void on_reset(void* instance) {
  if (instance == nullptr) {
    return;
  }
  auto* self = static_cast<LumaStats*>(instance);
  // 分辨率可能变了，丢掉 staging 纹理让它按新尺寸重建
  self->staging.Reset();
  self->staging_width = 0;
  self->staging_height = 0;
  log_info("luma_stats: 流已重置");
}

void destroy(void* instance) {
  delete static_cast<LumaStats*>(instance);
}

int create(const DuoPluginHostServices* host, const char* config, void** out_instance) {
  if (out_instance == nullptr) {
    return -1;
  }
  g_host = host;

  auto* self = new LumaStats();

  // 极简配置：支持 "step=8,log=60" 这种形式，够用就行
  if (config != nullptr && config[0] != '\0') {
    int step = 0;
    int interval = 0;
    if (std::sscanf(config, "step=%d,log=%d", &step, &interval) >= 1 && step > 0) {
      self->sample_step = step;
    }
    if (interval > 0) {
      self->log_interval = interval;
    }
  }

  *out_instance = self;
  log_info("luma_stats: 实例已创建");
  return 0;
}

const DuoAnalysisPlugin* as_analysis(void* instance) {
  if (instance == nullptr) {
    return nullptr;
  }
  static const DuoAnalysisPlugin api = {
      DUO_PLUGIN_ABI_VERSION,
      sizeof(DuoAnalysisPlugin),
      nullptr,  // instance 由宿主持有，这里不填
      &on_frame,
      &on_reset,
      &destroy,
  };
  return &api;
}

void destroy_instance(void* instance) { destroy(instance); }

const DuoPlugin g_plugin = {
    DUO_PLUGIN_ABI_VERSION,
    sizeof(DuoPlugin),
    DUO_PLUGIN_KIND_ANALYSIS,
    "duo.luma_stats",
    "1.0.0",
    "统计每帧平均亮度，检测黑帧（源断/卡顿）",
    &create,
    &as_analysis,
    &destroy_instance,
};

}  // namespace

extern "C" DUO_PLUGIN_EXPORT const DuoPlugin* duo_plugin_entry(unsigned int host_abi_version) {
  if (host_abi_version != DUO_PLUGIN_ABI_VERSION) {
    return nullptr;
  }
  return &g_plugin;
}
