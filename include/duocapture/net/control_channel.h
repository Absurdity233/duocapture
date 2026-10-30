#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "duocapture/common/types.h"

namespace duo {

// 控制通道消息类型。走独立端口，和视频流分开，避免和 1200 字节的
// 视频包混在一起排队。
enum class ControlType : u8 {
  Nack = 1,      // 接收端 → 发送端：请求重传若干包
  RequestIdr = 2,// 接收端 → 发送端：请求一个关键帧
  Heartbeat = 3, // 双向：保活 + 统计回传
};

#pragma pack(push, 1)
struct ControlHeader {
  u32 magic;   // kControlMagic
  u8 type;     // ControlType
  u8 count;    // 后续条目数（Nack 时是包数）
  u16 reserved;
};

// NACK 条目：一帧内连续的一段包。
// 用区间而不是逐个包，是因为丢包通常成串出现，区间能把消息压得很小。
struct NackRange {
  u32 frame_id;
  u16 begin;  // 起始 pkt_index（含）
  u16 end;    // 结束 pkt_index（含）
};
#pragma pack(pop)

inline constexpr u32 kControlMagic = 0x314C5443u;  // 'CTL1'
inline constexpr std::size_t kMaxNackRanges = 64;

// 把 (frame_id, pkt_index) 列表压成连续区间。
std::vector<NackRange> compact_nacks(const std::vector<std::pair<u32, u16>>& missing);

// 序列化 / 解析
Bytes serialize_control(ControlType type, ByteSpan payload);
bool parse_control(ByteSpan data, ControlHeader& header_out, ByteSpan& payload_out) noexcept;

}  // namespace duo
