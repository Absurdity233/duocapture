#pragma once

#include <cstddef>
#include <string>

#include "duocapture/common/types.h"

namespace duo {

// 接收端（A 端）配置。
//
// 与 SenderConfig 一样单独成文件、不依赖 D3D11，让配置解析能进可移植层。
struct ReceiverConfig {
  u16 listen_port = kDefaultVideoPort;

  // 0 = 跟随码流自动探测；非 0 时强制使用。
  u32 width = 0;
  u32 height = 0;

  // 抖动缓冲窗口（帧数）。局域网 4-8 足够；有丢包/乱序时调大。
  std::size_t jitter_window = 8;

  // NACK 缺失检测延迟（毫秒）。默认 3，前 16 个包用这个值，
  // 之后由抖动自适应接管（2–20 ms 伸缩）。
  u32 nack_delay_ms = 3;

  // false = 固定用上面的 nack_delay_ms，不让抖动估计改它。
  // 有线上抖动很小、想要确定性行为时用；固定值必须 > 0。
  bool nack_delay_adaptive = true;

  bool show_window = true;
  // false = 不等 vsync，用 tearing 换低延迟（默认）。开 vsync 画面更稳，
  // 但会多等 0-8 ms。
  bool vsync = false;

  // 分析 tap：把解码后的帧交给下游 CV/OCR。忙则丢帧，绝不阻塞呈现。
  bool enable_analysis = false;

  // 播放音频（A 端出声）。
  bool play_audio = true;

  // 在画面上叠加延迟/丢包统计。
  bool show_overlay = false;

  // 插件上报的检测框是否合成到画面上。
  bool boxes_overlay = true;

  // 发送端地址，用于回传 NACK / 请求关键帧。空表示不回传。
  std::string peer_host;
  u16 peer_control_port = kDefaultControlPort;
};

}  // namespace duo
