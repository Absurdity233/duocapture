#pragma once

#include <cstddef>
#include <string>

#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

namespace duo {

// 发送端（B 端）配置。
//
// 单独成文件、且不依赖任何 D3D11 类型，是为了让配置文件解析能放进
// 可移植层（duonet），在 Linux CI 上也能测。Sender 类本身是 Windows 专属的。
struct SenderConfig {
  u32 display_index = 0;
  bool prefer_wgc = true;

  u32 fps = 120;
  u32 bitrate_bps = 80'000'000;
  Codec codec = Codec::Hevc;

  std::string target_host = "127.0.0.1";
  u16 target_port = kDefaultVideoPort;

  std::size_t fec_group_size = 16;
  bool enable_fec = true;
  bool intra_refresh = true;
  u32 gop_frames = 0;

  // 限速：默认按目标码率再留 20% 余量，够 FEC 和包头开销。
  u64 pacer_bytes_per_second = 0;

  // 录到本地 MKV。零转码：直接把编码器输出封装进容器，不重编码。
  // 空字符串表示不录制。
  std::string record_path;

  // 推流地址（rtmp:// 或 srt://）。空表示不推流。
  // 走 ffmpeg 子进程 + 命名管道，-c copy 零转码。
  std::string push_url;
  std::string ffmpeg_path;

  // 控制通道端口（接收端往这里发 NACK / 请求关键帧）。
  u16 control_port = kDefaultControlPort;
  // 重传缓冲时长：保留最近这么多毫秒的包。太短会救不回迟到包，
  // 太长白占内存（80Mbps 下 200ms 约 2MB）。
  u32 retransmit_window_ms = 200;

  // 是否采集系统声音（WASAPI 环回）并混入录制/传输。
  bool capture_audio = false;
  u32 audio_bitrate_bps = 128'000;

  // 调试用：从 Annex-B 裸流回放（不采集、不编码），用于在没有持续
  // 画面来源时验证传输/解码/呈现链路。
  std::string playback_path;
  u32 playback_fps = 20;
};

}  // namespace duo
