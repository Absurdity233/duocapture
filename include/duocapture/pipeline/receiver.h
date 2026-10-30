#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/decode/decoder.h"
#include "duocapture/encode/encoder.h"
#include "duocapture/net/control_channel.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/transport.h"
#include "duocapture/audio/aac_decoder.h"
#include "duocapture/audio/wasapi_render.h"
#include "duocapture/pipeline/analysis_tap.h"
#include "duocapture/present/box_overlay.h"
#include "duocapture/present/presenter.h"

namespace duo {

struct ReceiverConfig {
  u16 listen_port = kDefaultVideoPort;

  // 0 = 跟随码流自动探测；非 0 时强制使用。
  u32 width = 0;
  u32 height = 0;

  // 抖动缓冲窗口（帧数）。局域网 4-8 足够；有丢包/乱序时调大。
  std::size_t jitter_window = 8;

  // NACK 缺失检测延迟。
  u32 nack_delay_ms = 3;

  bool show_window = true;
  bool vsync_off = true;  // tearing，延迟优先

  // 分析 tap：把解码后的帧交给下游 CV/OCR。忙则丢帧，绝不阻塞呈现。
  bool enable_analysis = false;

  // 播放音频（A 端出声）。
  bool play_audio = true;

  // 在画面上叠加延迟/丢包统计。
  bool show_overlay = false;

  // 发送端地址，用于回传 NACK / 请求关键帧。空表示不回传。
  std::string peer_host;
  u16 peer_control_port = kDefaultControlPort;
};

struct ReceiverStats {
  u64 datagrams_received = 0;
  u64 frames_assembled = 0;
  u64 frames_presented = 0;
  u64 frames_dropped = 0;
  u64 packets_recovered_by_fec = 0;
  u64 decoder_errors = 0;
  double receive_fps = 0.0;
  double end_to_end_ms = 0.0;

  // 分析 tap
  u64 frames_analyzed = 0;
  u64 frames_analysis_dropped = 0;

  // 音频
  u64 audio_frames = 0;
  u64 audio_decode_errors = 0;

  // 网络质量
  double arrival_jitter_ms = 0.0;
  u32 nack_delay_ms = 0;
  u64 nacks_issued = 0;

  // 延迟分布
  double p50_end_to_end_ms = 0.0;
  double p99_end_to_end_ms = 0.0;
};

// A 端接收管线：UDP → 抖动缓冲 → 解码 → 呈现。
//
// 呈现语义是"只显示最新帧"：解码出来的帧如果来不及显示就直接丢弃，
// 绝不排队。这是低延迟链路的核心约束。
class Receiver {
 public:
  Receiver();
  ~Receiver();

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  Status start(const ReceiverConfig& config);
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] ReceiverStats stats() const;

  // 窗口消息泵。show_window=false 时无窗口，此函数直接返回 true。
  bool pump();

  // 设置分析回调（须在 start 之前调用）。
  void set_analysis_callback(AnalysisTap::Callback callback, void* user);

  // 用插件适配器替代内建回调。传入后 enable_analysis 自动打开。
  // 适配器由调用方持有，生命周期必须覆盖 Receiver。
  // 若适配器支持检测框上报，Receiver 会把框合成到呈现画面上。
  using OverlayBoxFetchFn = int (*)(void* user, DuoOverlayBox* out, int max_boxes);
  void set_analysis_plugin(void* adapter, AnalysisTap::Callback thunk, void* user,
                           OverlayBoxFetchFn boxes_fetch = nullptr);

  // 检测框叠加的开关（默认开）。只在 set_analysis_plugin 后有效。
  void set_boxes_overlay_enabled(bool enabled) noexcept { boxes_overlay_enabled_ = enabled; }

  // 呈现器建好后回调，用于把 D3D 设备交给插件。
  void set_device_ready_callback(void (*fn)(void* user, void* device, void* context), void* user);

  [[nodiscard]] Win32Presenter* presenter() noexcept;

 private:
  void receive_loop();
  void handle_audio_packet(const Packet& packet);
  void render_loop();
  Status ensure_decoder(const AssembledFrame& first_frame, u32 width, u32 height);

  ReceiverConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> window_closed_{false};

  UdpSocket socket_;
  JitterBuffer jitter_;
  std::unique_ptr<IDecoder> decoder_;
  std::unique_ptr<Win32Presenter> presenter_;
  AnalysisTap analysis_;
  AnalysisTap::Callback analysis_callback_ = nullptr;
  void* analysis_user_ = nullptr;
  RollingStats latency_stats_{600};
  TelemetryOverlay overlay_;
  BoxOverlay box_overlay_;

  // 检测框来源：适配器的紧凑回调 + 它的 this。呈现线程通过它取最新框。
  static void draw_detection_boxes(void* user, ID3D11Texture2D* back_buffer, u32 width,
                                   u32 height);
  OverlayBoxFetchFn boxes_fetch_ = nullptr;
  void* boxes_fetch_user_ = nullptr;
  bool boxes_overlay_enabled_ = true;

  // 音频：按 frame_id 攒包，齐了就解
  AacDecoder audio_decoder_;
  WasapiRender audio_render_;
  bool audio_started_ = false;
  std::map<u32, std::vector<Packet>> audio_pending_;
  std::mutex audio_mutex_;

  // 控制通道（回传给发送端）
  UdpSocket control_socket_;
  u64 last_nack_ns_ = 0;
  u64 last_idr_request_ns_ = 0;
  void send_nacks();

  std::thread receive_thread_;
  std::thread render_thread_;

  // 解码后的待呈现帧。只保留最新一帧，旧的直接覆盖。
  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  DecodedFrame latest_{};
  bool has_frame_ = false;
  u64 latest_arrival_ns_ = 0;

  mutable std::mutex stats_mutex_;
  ReceiverStats stats_{};
  Codec codec_ = Codec::Hevc;
  u32 width_ = 0;
  u32 height_ = 0;
  bool decoder_configured_ = false;
  bool seen_keyframe_ = false;
  // set_analysis_plugin 设的强制标志：start() 里 config_ 会被整体覆盖，
  // 必须单独记住，否则插件永远收不到帧。
  bool analysis_forced_ = false;
  void (*device_ready_fn_)(void*, void*, void*) = nullptr;
  void* device_ready_user_ = nullptr;
  u32 frames_since_start_ = 0;
};

}  // namespace duo
