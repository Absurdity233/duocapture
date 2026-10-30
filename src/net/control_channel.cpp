#include "duocapture/net/control_channel.h"

#include <algorithm>
#include <cstring>

namespace duo {

std::vector<NackRange> compact_nacks(const std::vector<std::pair<u32, u16>>& missing) {
  std::vector<NackRange> out;
  if (missing.empty()) {
    return out;
  }

  // 输入通常已按 (frame_id, pkt_index) 有序，但别假设。
  std::vector<std::pair<u32, u16>> sorted = missing;
  std::sort(sorted.begin(), sorted.end());

  NackRange current{sorted[0].first, sorted[0].second, sorted[0].second};
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const auto& [frame_id, index] = sorted[i];
    // 同一帧且索引连续（或重叠）就并进当前区间
    if (frame_id == current.frame_id && index <= static_cast<u16>(current.end + 1)) {
      current.end = std::max<u16>(current.end, index);
      continue;
    }
    out.push_back(current);
    if (out.size() >= kMaxNackRanges) {
      return out;  // 超出上限就截断，剩下的等下一轮
    }
    current = NackRange{frame_id, index, index};
  }
  out.push_back(current);
  return out;
}

Bytes serialize_control(ControlType type, ByteSpan payload) {
  Bytes out(sizeof(ControlHeader) + payload.size());
  ControlHeader header{};
  header.magic = kControlMagic;
  header.type = static_cast<u8>(type);
  header.count = static_cast<u8>(payload.size() / sizeof(NackRange));
  std::memcpy(out.data(), &header, sizeof(header));
  if (!payload.empty()) {
    std::memcpy(out.data() + sizeof(header), payload.data(), payload.size());
  }
  return out;
}

bool parse_control(ByteSpan data, ControlHeader& header_out, ByteSpan& payload_out) noexcept {
  if (data.size() < sizeof(ControlHeader)) {
    return false;
  }
  ControlHeader header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.magic != kControlMagic) {
    return false;
  }
  header_out = header;
  payload_out = data.subspan(sizeof(ControlHeader));
  return true;
}

}  // namespace duo
