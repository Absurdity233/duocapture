#include "duocapture/net/fec.h"

#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace duo {
namespace {

using PaddedPayload = std::array<u8, kMaxPacketPayload>;

// 只对载荷做异或。包头在恢复时从同组兄弟包重建，不需要参与运算——
// 这样每个包的异或量从 1232 字节降到 1200 字节，且不用重复序列化包头。
void xor_payload(PaddedPayload& acc, ByteSpan payload) noexcept {
  const std::size_t n = payload.size();
  for (std::size_t i = 0; i < n; ++i) {
    acc[i] = static_cast<u8>(acc[i] ^ payload[i]);
  }
}

PaddedPayload pad_payload(ByteSpan payload) {
  PaddedPayload padded{};
  std::memcpy(padded.data(), payload.data(), payload.size());
  return padded;
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

    PaddedPayload acc{};
    for (std::size_t i = begin; i < end; ++i) {
      Packet& packet = packets[i];
      packet.header.fec_group = group;
      packet.header.fec_index = static_cast<u8>(i - begin);
      packet.header.fec_count = static_cast<u8>(span);
      xor_payload(acc, ByteSpan(packet.payload.data(), packet.payload.size()));
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
    // 若本组包含帧的最后一个包，把它的真实载荷长度整个放进 reserved。
    // 旧实现只用了低 8 位，尾包超过 255 字节时被截断，恢复出的帧会短一截
    // （800 字节的尾包被截成 32，一帧凭空少 768 字节）。
    // reserved 在数据包上存 codec，在校验包上没有别的用途，直接整字段用。
    const u16 tail_len =
        (end == count) ? static_cast<u16>(packets[count - 1].payload.size()) : 0;
    check.header.reserved = tail_len;
    check.payload.assign(acc.begin(), acc.end());
    parity.push_back(std::move(check));
  }

  return parity;
}

bool FecCodec::recover_group(const std::vector<const Packet*>& data, const Packet* parity,
                             u8 group_index, u16 group_base, std::size_t group_size,
                             u16& out_missing_index, Packet& out_recovered) {
  if (parity == nullptr || group_size < 2 || data.size() + 1 != group_size) {
    return false;
  }
  if (data.empty()) {
    return false;
  }
  if (group_size > 64) {
    return false;  // 位图只有 64 位；encode_frame 的组远小于这个上限
  }

  // 找出组内缺哪个槽位。用位图而非排序，避免分配。
  u64 seen = 0;
  std::size_t max_payload = 0;
  const Packet* sibling = nullptr;
  for (const Packet* packet : data) {
    const u8 slot = packet->header.fec_index;
    if (slot >= group_size) {
      return false;  // 数据不一致
    }
    seen |= (1ull << slot);
    if (packet->payload.size() > max_payload) {
      max_payload = packet->payload.size();
    }
    // 用 pkt_index 最小的那个兄弟包重建包头，保证结果稳定可复现
    // （data 的顺序取决于到达顺序，乱序时 front() 不一定是 0 号槽）。
    if (sibling == nullptr || packet->header.pkt_index < sibling->header.pkt_index) {
      sibling = packet;
    }
  }

  int missing = -1;
  for (std::size_t slot = 0; slot < group_size; ++slot) {
    if ((seen & (1ull << slot)) == 0) {
      missing = static_cast<int>(slot);
      break;
    }
  }
  // 槽位唯一性是恢复正确的前提：若两个包声称同一个槽位，那么总数虽然
  // 对得上，异或出来的却是另一个槽位的垃圾数据——而且会静默解码花屏。
  std::size_t distinct = 0;
  for (u64 bits = seen; bits != 0; bits &= bits - 1) {
    ++distinct;
  }
  if (distinct != data.size()) {
    return false;
  }
  if (missing < 0) {
    return false;
  }

  // 异或：组内已收到的载荷 XOR 校验载荷 = 缺失包的载荷
  PaddedPayload acc = pad_payload(ByteSpan(parity->payload.data(), parity->payload.size()));
  for (const Packet* packet : data) {
    xor_payload(acc, ByteSpan(packet->payload.data(), packet->payload.size()));
  }

  // 包头从同组兄弟包重建（除 pkt_index / fec_index / payload_len 外都相同）
  PacketHeader header = sibling->header;
  const int pkt_index = static_cast<int>(group_base) + missing;
  if (pkt_index < 0 || pkt_index >= static_cast<int>(header.pkt_count)) {
    return false;
  }

  const bool is_last = (pkt_index + 1 == static_cast<int>(header.pkt_count));
  const u32 payload_len =
      is_last ? static_cast<u32>(parity->header.reserved) : static_cast<u32>(max_payload);
  if (payload_len > kMaxPacketPayload) {
    return false;
  }

  header.flags = static_cast<u8>(header.flags & kFlagKeyFrame);
  if (pkt_index == 0) {
    header.flags |= kFlagFrameStart;
  }
  if (is_last) {
    header.flags |= kFlagFrameEnd;
  }
  header.fec_group = group_index;
  header.fec_index = static_cast<u8>(missing);
  header.pkt_index = static_cast<u16>(pkt_index);
  header.payload_len = payload_len;

  out_missing_index = static_cast<u16>(missing);
  out_recovered.header = header;
  out_recovered.payload.assign(acc.begin(),
                               acc.begin() + static_cast<std::ptrdiff_t>(payload_len));
  return true;
}

// 便捷版本，保留给测试。高频路径请用 recover_group。
bool FecCodec::recover_one(std::vector<Packet>& packets) {
  // 按 fec_group 分组
  std::map<u8, std::vector<const Packet*>> data_by_group;
  std::map<u8, const Packet*> parity_by_group;
  std::map<u8, std::size_t> group_size;

  for (const Packet& packet : packets) {
    if (packet.is_parity()) {
      parity_by_group[packet.header.fec_group] = &packet;
      group_size[packet.header.fec_group] = packet.header.fec_count;
    } else {
      data_by_group[packet.header.fec_group].push_back(&packet);
    }
  }

  for (const auto& [group, data] : data_by_group) {
    const auto parity_it = parity_by_group.find(group);
    if (parity_it == parity_by_group.end() || data.empty()) {
      continue;
    }
    // 组基址必须从包本身推：帧尾那组通常不满 group_size。
    u16 base = static_cast<u16>(data.front()->header.pkt_index -
                                data.front()->header.fec_index);
    for (const Packet* packet : data) {
      const u16 candidate =
          static_cast<u16>(packet->header.pkt_index - packet->header.fec_index);
      if (candidate < base) {
        base = candidate;
      }
    }
    u16 missing = 0;
    Packet recovered;
    if (recover_group(data, parity_it->second, group, base, group_size[group], missing, recovered)) {
      packets.push_back(std::move(recovered));
      return true;
    }
  }
  return false;
}

}  // namespace duo
