#pragma once

#include <atomic>
#include <fstream>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/capture/capture_source.h"
#include "duocapture/capture/color_converter.h"
#include "duocapture/common/ring_buffer.h"
#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"
#include "duocapture/net/pacer.h"
#include "duocapture/net/packet.h"
#include "duocapture/net/transport.h"

namespace duo {

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

  // 录到本地文件（M3 之前的调试手段，零转码直接写 Annex-B 裸流）。
  std::string record_path;

  // 调试用：从 Annex-B 裸流回放（不采集、不编码），用于在没有持续
  // 画面来源时验证传输/解码/呈现链路。
  std::string playback_path;
  u32 playback_fps = 20;
};

struct SenderStats {
  u64 frames_captured = 0;
  u64 frames_encoded = 0;
  u64 frames_sent = 0;
  u64 packets_sent = 0;
  u64 bytes_sent = 0;
  u64 encode_errors = 0;
  u64 capture_restarts = 0;
  double avg_encode_ms = 0.0;
  double fps = 0.0;
};

// B 端发送管线：采集 → 色彩转换 → 编码 → 打包 → 限速 → UDP。
//
// 线程划分：
//   capture: 等帧、转换、编码、打包入队（一条线程，因为这三步都是
//            GPU 顺序依赖的，拆开只会增加同步开销）
//   send:    从限速器取包并发送
class Sender {
 public:
  Sender();
  ~Sender();

  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  Status start(const SenderConfig& config);
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] SenderStats stats() const;
  [[nodiscard]] const SenderConfig& config() const noexcept { return config_; }

 private:
  void capture_loop();
  void send_loop();
  Status setup_pipeline();

  SenderConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  std::unique_ptr<ICaptureSource> capture_;
  ColorConverter converter_;
  std::unique_ptr<IEncoder> encoder_;
  UdpSocket socket_;
  Pacer pacer_;

  std::ofstream record_;
  std::thread capture_thread_;
  std::thread send_thread_;

  // 打包后的包队列，capture 线程生产、send 线程消费。
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::vector<Packet> pending_;

  mutable std::mutex stats_mutex_;
  SenderStats stats_{};
  u32 next_frame_id_ = 0;

  void* device_ = nullptr;
  void* context_ = nullptr;
  u32 width_ = 0;
  u32 height_ = 0;
};

}  // namespace duo
