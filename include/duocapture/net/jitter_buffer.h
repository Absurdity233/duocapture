#pragma once

#include <map>
#include <utility>
#include <vector>

#include "duocapture/common/clock.h"
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

  void insert(Packet packet) { insert_at(std::move(packet), Clock::now_ns()); }

  // 带显式到达时刻的版本。测试用它模拟稳定的包间隔，不必靠 sleep——
  // CI 上 sleep 自身的抖动比被测的到达抖动还大，测试会随机失败。
  void insert_at(Packet packet, u64 now_ns);

  // 取出最早的一帧完整数据。没有完整帧时返回 false。
  bool try_pop(AssembledFrame& out);

  // 返回需要重传的 (frame_id, pkt_index) 列表。
  std::vector<std::pair<u32, u16>> collect_nacks(u64 now_ns);

  // 周期性维护：对等满了 NACK 延迟、且只缺 1 个包的组做 FEC 恢复。
  // 接收循环应当定期调用（内部按 1ms 节流）。不放在 insert 里是因为
  // 包刚到就恢复会把「只是晚到」的包当成丢包，乱序链路上每个组都会
  // 被白白恢复一次。
  void service(u64 now_ns);

  [[nodiscard]] const JitterStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t pending_frames() const noexcept { return frames_.size(); }

  // 手动覆盖 NACK 延迟。0 = 交给自适应逻辑决定。
  void set_nack_delay(u32 delay_ms) noexcept { fixed_nack_delay_ms_ = delay_ms; }

 private:
  static constexpr std::size_t kNoParity = static_cast<std::size_t>(-1);

  // 单个 FEC 组的增量状态。
  //
  // 关键点是「收到几个」用计数维护，而不是每次去扫整帧：一帧 N 个包、
  // 每组 S 个，旧实现每收一个包都要遍历所有组、每组再扫一遍全部已收到
  // 的包，总复杂度 O(N²·N/S)。现在每个包只做 O(1) 计数，只有某组恰好
  // 缺 1 个包时才做一次 O(S) 的异或。
  //
  // received 存的是 Entry::data（std::map，节点地址稳定）里元素的指针，
  // 所以插入新包不会让它们失效。组号是 u8，一帧超过 256 组（默认
  // S=16 时即 4096 包）会回绕；检测到组基址变化时按新组重置。
  struct GroupState {
    u16 base = 0;   // 该组 0 号槽位对应的 pkt_index
    u16 expected = 0;  // 组内数据包应有数量（fec_count）
    std::vector<const Packet*> received;
    std::size_t parity_idx = kNoParity;  // 指向 Entry::parity 的下标
    bool base_known = false;
    bool given_up = false;  // 已恢复成功，或确认本组救不回来
  };

  struct Entry {
    std::map<u16, Packet> data;
    std::vector<Packet> parity;
    u16 pkt_count = 0;
    u64 capture_ts_ns = 0;
    bool key = false;
    u64 first_seen_ns = 0;
    u64 last_nack_ns = 0;
    bool complete = false;

    std::map<u8, GroupState> groups;
  };

  void try_recover_group(Entry& entry, u8 group);
  // 到达 NACK 期限后，把只缺 1 个包的组用 FEC 补上。
  // 不在包到达时立刻恢复：那时缺的那个包很可能只是还在路上，抢先恢复
  // 既浪费校验包，又会在乱序链路上把每个组都"恢复"一遍。
  void recover_ripe_groups(Entry& entry, u64 now_ns);
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
  // 上次周期恢复的时刻，避免每个包都扫一遍所有未完成帧的组表。
  u64 last_recover_ns_ = 0;
  JitterStats stats_{};
};

}  // namespace duo
