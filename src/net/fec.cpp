#include "duocapture/net/fec.h"

#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace duo {
namespace {

using PaddedPacket = std::array<u8, kMaxPacketSize>;

PaddedPacket pad_packet(const Packet& packet) {
  PaddedPacket padded{};
  const Bytes raw = serialize(packet.header, packet.payload);
  std::memcpy(padded.data(), raw.data(), raw.size());
  return padded;
}

void xor_into(PaddedPacket& acc, const PaddedPacket& other) noexcept {
  for (std::size_t i = 0; i < kMaxPacketSize; ++i) {
    acc[i] = static_cast<u8>(acc[i] ^ other[i]);
  }
}

}  // namespace

std::vector<Packet> FecCodec::encode_frame(std::vector<Packet>& packets, std::size_t group_size) {
  std::vector<Packet> parity;
  if (group_size < 2 || packets.empty()) {
    return parity;
  }

  const std::size_t count = packets.size();

  for (std::size_t begin = 0; begin < count; begin += group_size) {
    const std::size_t end = (begin + group_size < count) ? begin + group_size : count;
    const std::size_t span = end - begin;
    const u8 group = static_cast<u8>((begin / group_size) & 0xFF);

    PaddedPacket acc{};
    for (std::size_t i = begin; i < end; ++i) {
      Packet& packet = packets[i];
      packet.header.fec_group = group;
      packet.header.fec_index = static_cast<u8>(i - begin);
      packet.header.fec_count = static_cast<u8>(span);
      xor_into(acc, pad_packet(packet));
    }

    Packet check;
    check.header = packets[begin].header;
    check.header.flags =
        static_cast<u8>((check.header.flags & ~(kFlagFrameStart | kFlagFrameEnd)) | kFlagFecParity);
    check.header.fec_group = group;
    check.header.fec_index = static_cast<u8>(span);  // == fec_count 表示校验包
    check.header.fec_count = static_cast<u8>(span);
    check.header.pkt_index = static_cast<u16>(kParityIndexBase + group);
    check.header.payload_len = static_cast<u32>(kMaxPacketPayload);
    // 若本组包含帧的最后一个包，把它的真实载荷长度放在 reserved 里，
    // 否则恢复出的尾包无法确定该截断到哪里。
    check.header.reserved =
        (end == count) ? static_cast<u16>(packets[count - 1].payload.size()) : 0;
    check.payload.assign(acc.begin() + static_cast<std::ptrdiff_t>(kPacketHeaderSize), acc.end());
    parity.push_back(std::move(check));
  }

  return parity;
}

bool FecCodec::recover_one(std::vector<Packet>& packets) {
  // 按 fec_group 收集数据包与校验包。
  std::map<u8, std::vector<std::size_t>> data_by_group;
  std::map<u8, std::size_t> parity_index;
  std::map<u8, int> group_size;

  for (std::size_t i = 0; i < packets.size(); ++i) {
    const PacketHeader& h = packets[i].header;
    if (h.flags & kFlagFecParity) {
      parity_index[h.fec_group] = i;
      group_size[h.fec_group] = static_cast<int>(h.fec_count);
    }
  }
  for (std::size_t i = 0; i < packets.size(); ++i) {
    const PacketHeader& h = packets[i].header;
    if ((h.flags & kFlagFecParity) == 0) {
      data_by_group[h.fec_group].push_back(i);
    }
  }

  for (const auto& [group, indices] : data_by_group) {
    const auto parity_it = parity_index.find(group);
    if (parity_it == parity_index.end()) {
      continue;
    }
    const int expected = group_size[group];
    if (expected <= 1) {
      continue;
    }
    // 只处理恰好缺 1 个包的组。
    if (static_cast<int>(indices.size()) + 1 != expected) {
      continue;
    }

    std::vector<bool> seen(static_cast<std::size_t>(expected), false);
    for (const std::size_t idx : indices) {
      const u8 slot = packets[idx].header.fec_index;
      if (slot < static_cast<u8>(expected)) {
        seen[slot] = true;
      }
    }

    int missing = -1;
    for (int slot = 0; slot < expected; ++slot) {
      if (!seen[static_cast<std::size_t>(slot)]) {
        missing = slot;
        break;
      }
    }
    if (missing < 0) {
      continue;
    }

    // 校验只覆盖载荷，所以恢复出来的是缺失包的载荷；
    // 包头从同组的兄弟包重建（除 pkt_index / fec_index / payload_len 外都相同）。
    PaddedPacket acc{};
    for (const std::size_t idx : indices) {
      xor_into(acc, pad_packet(packets[idx]));
    }
    xor_into(acc, pad_packet(packets[parity_it->second]));

    const Packet& sibling = packets[indices.front()];
    const Packet& parity_packet = packets[parity_it->second];

    PacketHeader header = sibling.header;
    const int base = static_cast<int>(group) * expected;
    const int pkt_index = base + missing;
    if (pkt_index < 0 || pkt_index >= static_cast<int>(header.pkt_count)) {
      continue;
    }

    const bool is_last = (pkt_index + 1 == static_cast<int>(header.pkt_count));
    const u32 payload_len =
        is_last ? static_cast<u32>(parity_packet.header.reserved)
                : static_cast<u32>(sibling.payload.size());
    if (payload_len > kMaxPacketPayload) {
      continue;
    }

    header.flags = static_cast<u8>(header.flags & kFlagKeyFrame);
    if (pkt_index == 0) {
      header.flags |= kFlagFrameStart;
    }
    if (is_last) {
      header.flags |= kFlagFrameEnd;
    }
    header.fec_index = static_cast<u8>(missing);
    header.pkt_index = static_cast<u16>(pkt_index);
    header.payload_len = payload_len;

    Packet recovered;
    recovered.header = header;
    recovered.payload.assign(acc.begin() + static_cast<std::ptrdiff_t>(kPacketHeaderSize),
                             acc.begin() +
                                 static_cast<std::ptrdiff_t>(kPacketHeaderSize + payload_len));
    packets.push_back(std::move(recovered));
    return true;
  }

  return false;
}

}  // namespace duo
