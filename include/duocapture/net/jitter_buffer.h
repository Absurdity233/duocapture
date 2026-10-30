#pragma once

#include <map>
#include <utility>
#include <vector>

#include "duocapture/common/types.h"
#include "duocapture/net/packet.h"

namespace duo {

struct AssembledFrame {
  u32 frame_id = 0;
  u64 capture_ts_ns = 0;
  bool key = false;
  u32 width = 0;
  u32 height = 0;
  Bytes data;
};

struct JitterStats {
  u64 packets_received = 0;
  u64 packets_recovered = 0;
  u64 frames_assembled = 0;
  u64 frames_dropped = 0;
  u64 nacks_issued = 0;

  // 到达间隔抖动（毫秒）。用 RFC 3550 的指数滑动平均：
  //   J += (|D| - J) / 16
  // 其中 D 是相邻包到达间隔与期望间隔之差。有线网络这个值通常
  // < 0.5ms，WiFi 会到 5-20ms。
  double arrival_jitter_ms = 0.0;

  // 当前生效的 NACK 延迟（毫秒），随抖动自适应。
  u32 effective_nack_delay_ms = 0;
};

// 接收端抖动缓冲。
//
// 职责：
//   1. 按 frame_id / pkt_index 重排乱序到达的包
//   2. 用 FEC 校验包就地恢复单个丢包
//   3. 对超时仍缺失的包产生 NACK 请求
//   4. 用窗口淘汰掉永远补不齐的帧，避免内存无限增长
class JitterBuffer {
 public:
  explicit JitterBuffer(std::size_t window_frames = 16, u32 nack_delay_ms = 3);

  void insert(Packet packet);

  // 取出最早的一帧完整数据。没有完整帧时返回 false。
  bool try_pop(AssembledFrame& out);

  // 返回需要重传的 (frame_id, pkt_index) 列表。
  std::vector<std::pair<u32, u16>> collect_nacks(u64 now_ns);

  [[nodiscard]] const JitterStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t pending_frames() const noexcept { return frames_.size(); }

  // 手动覆盖 NACK 延迟。0 = 交给自适应逻辑决定。
  void set_nack_delay(u32 delay_ms) noexcept { fixed_nack_delay_ms_ = delay_ms; }

 private:
  struct Entry {
    std::map<u16, Packet> data;
    std::vector<Packet> parity;
    u16 pkt_count = 0;
    u64 capture_ts_ns = 0;
    bool key = false;
    u64 first_seen_ns = 0;
    u64 last_nack_ns = 0;
    bool complete = false;
  };

  void try_recover(Entry& entry);
  void evict_if_needed(u32 current_frame_id);

  void update_arrival_jitter(u64 now_ns);

  std::size_t window_frames_;
  u32 nack_delay_ms_;
  u32 fixed_nack_delay_ms_ = 0;

  // 到达间隔统计
  u64 last_arrival_ns_ = 0;
  double expected_interval_ns_ = 0.0;
  double jitter_ns_ = 0.0;
  u64 jitter_samples_ = 0;
  std::map<u32, Entry> frames_;
  // 已交付的最大 frame_id，用于丢弃延迟到达的冗余包。
  u32 delivered_max_id_ = 0;
  bool delivered_valid_ = false;
  JitterStats stats_{};
};

}  // namespace duo
