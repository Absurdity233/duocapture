#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <vector>

#include "duocapture/capture/capture_source.h"
#include "duocapture/capture/color_converter.h"
#include "duocapture/common/ring_buffer.h"
#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"
#include "duocapture/net/control_channel.h"
#include "duocapture/net/pacer.h"
#include "duocapture/net/packet.h"
#include "duocapture/net/transport.h"
#include "duocapture/pipeline/sender_config.h"
#include "duocapture/audio/aac_encoder.h"
#include "duocapture/audio/wasapi_loopback.h"
#include "duocapture/record/mkv_muxer.h"
#include "duocapture/record/stream_pusher.h"

namespace duo {

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
  void audio_loop();
  void control_loop();
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

  MkvMuxer muxer_;
  StreamPusher pusher_;
  WasapiLoopback audio_capture_;
  AacEncoder audio_encoder_;
  std::thread audio_thread_;
  std::thread control_thread_;

  // 重传缓冲：(frame_id, pkt_index) → 已序列化的包字节
  struct RetainedPacket {
    Bytes wire;
    u64 sent_ns = 0;
  };
  std::mutex retain_mutex_;
  std::map<std::pair<u32, u16>, RetainedPacket> retained_;
  UdpSocket control_socket_;
  // 音频时间轴锚点：第一个视频帧的采集时间戳，音频按它对齐。
  std::atomic<u64> audio_anchor_ns_{0};
  // 音频包用自己的 frame_id 空间，避免和视频帧号混淆
  std::atomic<u32> next_audio_id_{0};
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
