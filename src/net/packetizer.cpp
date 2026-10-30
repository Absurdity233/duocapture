#include "duocapture/net/packetizer.h"

#include <algorithm>

namespace duo {

Packetizer::Packetizer(std::size_t mtu_payload)
    : mtu_payload_(mtu_payload == 0 ? kMaxPacketPayload : mtu_payload) {
  if (mtu_payload_ > kMaxPacketPayload) {
    mtu_payload_ = kMaxPacketPayload;
  }
}

std::vector<Packet> Packetizer::packetize(const FrameMeta& meta, ByteSpan frame) const {
  std::vector<Packet> out;

  const std::size_t total = frame.size();
  const std::size_t count = total == 0 ? 1 : (total + mtu_payload_ - 1) / mtu_payload_;
  if (count > 0xFFFF) {
    return out;  // 单帧过大，调用方应报错
  }

  out.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t offset = i * mtu_payload_;
    const std::size_t remain = total > offset ? total - offset : 0;
    const std::size_t len = std::min(mtu_payload_, remain);

    Packet packet;
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.flags = 0;
    if (meta.key) {
      packet.header.flags |= kFlagKeyFrame;
    }
    if (i == 0) {
      packet.header.flags |= kFlagFrameStart;
    }
    if (i + 1 == count) {
      packet.header.flags |= kFlagFrameEnd;
    }
    packet.header.fec_group = 0;
    packet.header.fec_index = 0;
    packet.header.fec_count = 0;
    packet.header.reserved = static_cast<u16>(static_cast<u16>(meta.codec) << 8);
    packet.header.frame_id = meta.frame_id;
    packet.header.pkt_index = static_cast<u16>(i);
    packet.header.pkt_count = static_cast<u16>(count);
    packet.header.capture_ts_ns = meta.capture_ts_ns;
    packet.header.payload_len = static_cast<u32>(len);

    packet.payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(offset),
                          frame.begin() + static_cast<std::ptrdiff_t>(offset + len));
    out.push_back(std::move(packet));
  }

  return out;
}

}  // namespace duo
