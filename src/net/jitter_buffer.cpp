#include "duocapture/net/jitter_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "duocapture/common/clock.h"
#include "duocapture/net/fec.h"
#include "duocapture/common/log.h"

namespace duo {

JitterBuffer::JitterBuffer(std::size_t window_frames, u32 nack_delay_ms)
    : window_frames_(window_frames == 0 ? 1 : window_frames), nack_delay_ms_(nack_delay_ms) {
  stats_.effective_nack_delay_ms = nack_delay_ms;
}

void JitterBuffer::update_arrival_jitter(u64 now_ns) {
  if (last_arrival_ns_ == 0) {
    last_arrival_ns_ = now_ns;
    return;
  }
  const double interval = static_cast<double>(now_ns - last_arrival_ns_);
  last_arrival_ns_ = now_ns;

  // 前 16 个包用来建立"期望间隔"的基线，之后才开始算抖动。
  // 直接用第一个间隔当基线会把启动时的突发误判成巨大抖动。
  if (jitter_samples_ < 16) {
    expected_interval_ns_ = (expected_interval_ns_ * static_cast<double>(jitter_samples_) +
                             interval) /
                            static_cast<double>(jitter_samples_ + 1);
    ++jitter_samples_;
    return;
  }

  // RFC 3550 的抖动估计：J += (|D| - J) / 16
  const double deviation = std::abs(interval - expected_interval_ns_);
  jitter_ns_ += (deviation - jitter_ns_) / 16.0;

  // 期望间隔也缓慢跟随（网络条件变化时能收敛）
  expected_interval_ns_ += (interval - expected_interval_ns_) / 64.0;

  stats_.arrival_jitter_ms = jitter_ns_ / 1e6;

  // NACK 延迟自适应：抖动越大，等得越久（给乱序包更多时间自己到齐，
  // 少发无效 NACK）。下限 2ms，上限 20ms。
  //   delay = clamp(2 + 2 * jitter, 2, 20)
  if (fixed_nack_delay_ms_ != 0) {
    stats_.effective_nack_delay_ms = fixed_nack_delay_ms_;
  } else {
    const double jitter_ms = jitter_ns_ / 1e6;
    const double adaptive = 2.0 + 2.0 * jitter_ms;
    const double clamped = adaptive < 2.0 ? 2.0 : (adaptive > 20.0 ? 20.0 : adaptive);
    stats_.effective_nack_delay_ms = static_cast<u32>(clamped);
  }
}

void JitterBuffer::insert(Packet packet) {
  ++stats_.packets_received;
  update_arrival_jitter(Clock::now_ns());

  const u32 frame_id = packet.header.frame_id;

  // 已经交付过的帧不再接受新包。否则延迟到达的冗余包会重建一个空 entry：
  // pkt_count 从包里恢复成 40、data 为 0，随后被当作"未完成帧"淘汰，
  // 日志上表现为莫名其妙的 0/40，同时白白占满窗口。
  //
  // 判定用"已交付的 frame_id 集合 + 单调下界"：frame_id 是发送端单调
  // 递增的，凡是 <= 已交付最大值的都视为过期。乱序允许在窗口内，
  // 但不会跨过已交付的帧。
  if (delivered_valid_ && static_cast<i64>(frame_id) - static_cast<i64>(delivered_max_id_) <= 0) {
    return;
  }

  Entry& entry = frames_[frame_id];

  if (entry.pkt_count == 0) {
    entry.pkt_count = packet.header.pkt_count;
    entry.capture_ts_ns = packet.header.capture_ts_ns;
    entry.key = packet.is_key();
    entry.first_seen_ns = Clock::now_ns();
  }

  if (packet.is_parity()) {
    const u8 group = packet.header.fec_group;
    const u8 span = packet.header.fec_count;
    entry.parity.push_back(std::move(packet));
    if (span >= 2) {
      GroupState& state = entry.groups[group];
      if (state.expected != 0 && state.expected != span) {
        // 同一组号出现两种跨度，说明帧内包数超过了 u8 组号能表示的
        // 范围、组号回绕了。按新的一组重新统计。
        state = GroupState{};
      }
      state.expected = span;
      state.parity_idx = entry.parity.size() - 1;
    }
  } else {
    const u16 pkt_index = packet.header.pkt_index;
    const auto [slot, inserted] = entry.data.emplace(pkt_index, std::move(packet));
    if (inserted) {
      const Packet& stored = slot->second;
      if (stored.header.fec_count >= 2 && stored.header.fec_index < stored.header.fec_count) {
        GroupState& state = entry.groups[stored.header.fec_group];
        const u16 base = static_cast<u16>(pkt_index - stored.header.fec_index);
        if ((state.base_known && state.base != base) ||
            (state.expected != 0 && state.expected != stored.header.fec_count)) {
          // 组号回绕：保留已收到的校验包下标，否则这组永远恢复不了。
          const std::size_t keep_parity = state.parity_idx;
          state = GroupState{};
          state.parity_idx = keep_parity;
        }
        if (!state.base_known) {
          state.base = base;
          state.base_known = true;
        }
        state.expected = stored.header.fec_count;
        // std::map 的节点地址稳定：之后 emplace 别的包不会让这个指针失效。
        state.received.push_back(&stored);
      }
    }
  }

  if (!entry.complete && entry.pkt_count != 0 && entry.data.size() >= entry.pkt_count) {
    entry.complete = true;
  }

  evict_if_needed(frame_id);
}

// 单组 FEC 恢复。
//
// 调用方保证该组恰好缺 1 个包（收到 expected-1 个），所以这里只做一次
// O(group_size) 的异或：不扫整帧、不复制已有包。恢复成功或确认失败后
// 都置 given_up，该组之后不再参与。
void JitterBuffer::try_recover_group(Entry& entry, u8 group) {
  const auto it = entry.groups.find(group);
  if (it == entry.groups.end()) {
    return;
  }
  GroupState& state = it->second;
  if (state.given_up || !state.base_known || state.parity_idx == kNoParity ||
      state.expected == 0 || state.received.size() + 1 != state.expected) {
    return;
  }

  u16 missing_index = 0;
  Packet recovered;
  if (!FecCodec::recover_group(state.received, &entry.parity[state.parity_idx], group, state.base,
                               state.expected, missing_index, recovered)) {
    state.given_up = true;
    return;
  }

  const u16 pkt_index = recovered.header.pkt_index;
  if (entry.data.emplace(pkt_index, std::move(recovered)).second) {
    ++stats_.packets_recovered;
  }
  state.given_up = true;
}

// 等满 NACK 延迟之后再做 FEC 恢复。
//
// 直接原因是乱序：包到达即恢复会把「只是晚到」的包当成丢包。selftest 里
// 丢 3 个包却触发了 10 次恢复，恢复出的包随后又被真包顶掉，纯属浪费。
// 代价是恢复出的包要等到期才出现在 data 里，但送进解码器的时间本来
// 也不会早于 NACK 期限——再早就是拿延迟换画面了。
void JitterBuffer::recover_ripe_groups(Entry& entry, u64 now_ns) {
  if (entry.complete || entry.groups.empty() || entry.pkt_count == 0) {
    return;
  }
  const u64 delay_ns = static_cast<u64>(stats_.effective_nack_delay_ms) * 1'000'000ull;
  if (entry.first_seen_ns != 0 && now_ns - entry.first_seen_ns < delay_ns) {
    return;
  }

  for (auto& [group, state] : entry.groups) {
    if (state.given_up || !state.base_known || state.parity_idx == kNoParity) {
      continue;
    }
    // 缺 2 个以上时不标记 given_up：NACK 补回一个之后这一组就重新变得
    // 可恢复了，那是最值得救的场景。这里只是本轮跳过，下次 service 重看。
    if (state.received.size() + 1 == state.expected) {
      try_recover_group(entry, group);
    }
    if (entry.data.size() >= entry.pkt_count) {
      entry.complete = true;
      return;
    }
  }
}

void JitterBuffer::evict_if_needed(u32 current_frame_id) {
  while (frames_.size() > window_frames_) {
    auto victim = frames_.begin();
    if (victim->first == current_frame_id && frames_.size() > 1) {
      ++victim;
    }
    if (!victim->second.complete) {
      ++stats_.frames_dropped;
      DUO_WARN("淘汰未完成帧 %u: %zu/%u 包, 校验包 %zu", victim->first,
               victim->second.data.size(), victim->second.pkt_count,
               victim->second.parity.size());
    }
    frames_.erase(victim);
  }
}

// 周期恢复。按 1ms 节流：调用方（接收循环）可能在每个包之后都调，
// 但组表只在时间前进时才扫。
void JitterBuffer::service(u64 now_ns) {
  if (last_recover_ns_ != 0 && now_ns - last_recover_ns_ < 1'000'000ull) {
    return;
  }
  last_recover_ns_ = now_ns;

  for (auto& [frame_id, entry] : frames_) {
    // 组表为空（没开 FEC）或早已完成的帧直接跳过。
    if (entry.complete || entry.groups.empty()) {
      continue;
    }
    recover_ripe_groups(entry, now_ns);
  }
}

bool JitterBuffer::try_pop(AssembledFrame& out) {
  for (auto it = frames_.begin(); it != frames_.end(); ++it) {
    Entry& entry = it->second;
    if (entry.pkt_count == 0 || entry.data.size() < entry.pkt_count) {
      continue;
    }

    out.frame_id = it->first;
    out.capture_ts_ns = entry.capture_ts_ns;
    out.key = entry.key;
    out.data.clear();

    std::size_t total = 0;
    for (const auto& [index, packet] : entry.data) {
      total += packet.payload.size();
    }
    out.data.reserve(total);
    for (const auto& [index, packet] : entry.data) {
      out.data.insert(out.data.end(), packet.payload.begin(), packet.payload.end());
    }

    ++stats_.frames_assembled;
    delivered_max_id_ = it->first;
    delivered_valid_ = true;
    frames_.erase(it);
    return true;
  }
  return false;
}

std::vector<std::pair<u32, u16>> JitterBuffer::collect_nacks(u64 now_ns) {
  std::vector<std::pair<u32, u16>> missing;
  const u64 delay_ns = static_cast<u64>(stats_.effective_nack_delay_ms) * 1'000'000ull;

  for (auto& [frame_id, entry] : frames_) {
    if (entry.complete || entry.pkt_count == 0) {
      continue;
    }
    if (now_ns - entry.first_seen_ns < delay_ns) {
      continue;
    }
    // 同一帧最多每 5 ms 请求一次，避免 NACK 风暴。
    if (entry.last_nack_ns != 0 && now_ns - entry.last_nack_ns < 5'000'000ull) {
      continue;
    }

    // data 按 pkt_index 有序，顺序走一遍就能找出所有空洞，
    // 不用对每个序号做一次 map 查找（旧实现是 O(N log N)）。
    const std::size_t before = missing.size();
    u16 next = 0;
    for (const auto& [index, packet] : entry.data) {
      if (index >= entry.pkt_count) {
        break;
      }
      while (next < index) {
        missing.emplace_back(frame_id, next++);
      }
      next = static_cast<u16>(index + 1);
    }
    while (next < entry.pkt_count) {
      missing.emplace_back(frame_id, next++);
    }

    // 只给「确实有缺包」的帧打时间戳，否则一帧缺包会把同轮其他
    // 本来就完整的帧也标记成刚发过 NACK。
    if (missing.size() != before) {
      entry.last_nack_ns = now_ns;
      ++stats_.nacks_issued;
    }
  }

  return missing;
}

}  // namespace duo
