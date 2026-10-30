#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/decode/decoder.h"
#include "duocapture/encode/encoder.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/transport.h"
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

  [[nodiscard]] Win32Presenter* presenter() noexcept;

 private:
  void receive_loop();
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
  u32 frames_since_start_ = 0;
};

}  // namespace duo
