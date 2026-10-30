#pragma once

#include <vector>

#include "duocapture/common/types.h"
#include "duocapture/net/packet.h"

namespace duo {

struct FrameMeta {
  u32 frame_id = 0;
  u64 capture_ts_ns = 0;
  bool key = false;
  u32 width = 0;
  u32 height = 0;
};

// 把一帧编码数据切成定长载荷的 UDP 包。
// 不负责 FEC——FEC 字段由 fec_encode_frame 填充。
class Packetizer {
 public:
  explicit Packetizer(std::size_t mtu_payload = kMaxPacketPayload);

  [[nodiscard]] std::vector<Packet> packetize(const FrameMeta& meta, ByteSpan frame) const;

  [[nodiscard]] std::size_t mtu_payload() const noexcept { return mtu_payload_; }

 private:
  std::size_t mtu_payload_;
};

}  // namespace duo
