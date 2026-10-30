#include "duocapture/net/jitter_buffer.h"

#include <algorithm>

#include "duocapture/common/clock.h"
#include "duocapture/net/fec.h"

namespace duo {

JitterBuffer::JitterBuffer(std::size_t window_frames, u32 nack_delay_ms)
    : window_frames_(window_frames == 0 ? 1 : window_frames), nack_delay_ms_(nack_delay_ms) {}

void JitterBuffer::insert(Packet packet) {
  ++stats_.packets_received;

  const u32 frame_id = packet.header.frame_id;
  Entry& entry = frames_[frame_id];

  if (entry.pkt_count == 0) {
    entry.pkt_count = packet.header.pkt_count;
    entry.capture_ts_ns = packet.header.capture_ts_ns;
    entry.key = packet.is_key();
    entry.first_seen_ns = Clock::now_ns();
  }

  if (packet.is_parity()) {
    entry.parity.push_back(std::move(packet));
  } else {
    entry.data.emplace(packet.header.pkt_index, std::move(packet));
  }

  try_recover(entry);

  if (!entry.complete && entry.pkt_count != 0 && entry.data.size() >= entry.pkt_count) {
    entry.complete = true;
  }

  evict_if_needed(frame_id);
}

// 注意：这里在收到包的当下就尝试 FEC 恢复。若链路存在乱序，可能会把
// "只是晚到"的包当成丢包恢复出来，白白消耗一个校验包。局域网乱序罕见，
// 影响很小；生产版本应改为到达 NACK 期限之后才做 FEC 恢复。
void JitterBuffer::try_recover(Entry& entry) {
  if (entry.complete || entry.parity.empty() || entry.pkt_count == 0) {
    return;
  }
  if (entry.data.size() >= entry.pkt_count) {
    return;
  }

  // 把当前已收到的包拼成一个工作集交给 FEC 解码器。
  std::vector<Packet> working;
  working.reserve(entry.data.size() + entry.parity.size());
  for (const auto& [index, packet] : entry.data) {
    working.push_back(packet);
  }
  for (const Packet& check : entry.parity) {
    working.push_back(check);
  }

  bool recovered_any = false;
  while (FecCodec::recover_one(working)) {
    recovered_any = true;
  }
  if (!recovered_any) {
    return;
  }

  for (Packet& packet : working) {
    if (packet.is_parity()) {
      continue;
    }
    if (entry.data.find(packet.header.pkt_index) == entry.data.end()) {
      ++stats_.packets_recovered;
      entry.data.emplace(packet.header.pkt_index, std::move(packet));
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
    }
    frames_.erase(victim);
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
    frames_.erase(it);
    return true;
  }
  return false;
}

std::vector<std::pair<u32, u16>> JitterBuffer::collect_nacks(u64 now_ns) {
  std::vector<std::pair<u32, u16>> missing;
  const u64 delay_ns = static_cast<u64>(nack_delay_ms_) * 1'000'000ull;

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

    for (u16 index = 0; index < entry.pkt_count; ++index) {
      if (entry.data.find(index) == entry.data.end()) {
        missing.emplace_back(frame_id, index);
      }
    }
    if (!missing.empty()) {
      entry.last_nack_ns = now_ns;
      ++stats_.nacks_issued;
    }
  }

  return missing;
}

}  // namespace duo
