#pragma once

#include "duocapture/common/types.h"

namespace duo {

// ---- 包头标志位 -------------------------------------------------------------
inline constexpr u8 kFlagKeyFrame = 0x01;   // 该帧为关键帧（IDR）
inline constexpr u8 kFlagFrameStart = 0x02; // 该包是帧的第一个包
inline constexpr u8 kFlagFrameEnd = 0x04;   // 该包是帧的最后一个包
inline constexpr u8 kFlagFecParity = 0x08;  // 该包是 FEC 校验包

// 校验包的 pkt_index 从这里开始编号，与数据包区分开。
inline constexpr u16 kParityIndexBase = 0xF000;

// ---- 线格式 -----------------------------------------------------------------
// 全部字段为小端。x86/ARM64 均为小端，直接 memcpy 即可。
#pragma pack(push, 1)
struct PacketHeader {
  u32 magic;          // kProtocolMagic
  u16 version;        // kProtocolVersion
  u8 flags;           // kFlag* 位组合
  u8 fec_group;       // 帧内 FEC 组序号
  u8 fec_index;       // 组内序号；等于 fec_count 表示这是校验包
  u8 fec_count;       // 该组的数据包数量（0 表示未启用 FEC）
  u16 reserved;       // 高 8 位：Codec（0=h264 1=hevc 2=av1）；低 8 位：FEC 校验包的尾包长度
  u32 frame_id;       // 帧序号，单调递增
  u16 pkt_index;      // 帧内包序号（数据包）；校验包用 kParityIndexBase + 组号
  u16 pkt_count;      // 该帧的数据包总数
  u64 capture_ts_ns;  // 采集时刻（发送端 Clock）
  u32 payload_len;    // 载荷字节数
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == kPacketHeaderSize, "PacketHeader must be 32 bytes");

struct Packet {
  PacketHeader header{};
  Bytes payload;

  [[nodiscard]] std::size_t total_size() const noexcept {
    return kPacketHeaderSize + payload.size();
  }
  [[nodiscard]] bool is_parity() const noexcept { return (header.flags & kFlagFecParity) != 0; }
  [[nodiscard]] bool is_key() const noexcept { return (header.flags & kFlagKeyFrame) != 0; }
};

// 序列化为连续字节（包头 + 载荷）。
Bytes serialize(const PacketHeader& header, ByteSpan payload);

// 从连续字节解析。成功时 payload 指向 data 内部的子区间（零拷贝）。
bool parse(ByteSpan data, PacketHeader& header_out, ByteSpan& payload_out) noexcept;

}  // namespace duo
