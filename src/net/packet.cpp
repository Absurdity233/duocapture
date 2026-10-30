#include "duocapture/net/packet.h"

#include <cstring>

namespace duo {

Bytes serialize(const PacketHeader& header, ByteSpan payload) {
  Bytes out(kPacketHeaderSize + payload.size());
  std::memcpy(out.data(), &header, kPacketHeaderSize);
  if (!payload.empty()) {
    std::memcpy(out.data() + kPacketHeaderSize, payload.data(), payload.size());
  }
  return out;
}

bool parse(ByteSpan data, PacketHeader& header_out, ByteSpan& payload_out) noexcept {
  if (data.size() < kPacketHeaderSize) {
    return false;
  }

  PacketHeader header{};
  std::memcpy(&header, data.data(), kPacketHeaderSize);

  if (header.magic != kProtocolMagic) {
    return false;
  }
  if (header.payload_len > kMaxPacketPayload) {
    return false;
  }
  if (data.size() < kPacketHeaderSize + header.payload_len) {
    return false;
  }

  header_out = header;
  payload_out = data.subspan(kPacketHeaderSize, header.payload_len);
  return true;
}

}  // namespace duo
