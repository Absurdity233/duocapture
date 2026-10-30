#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace duo {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using Bytes = std::vector<u8>;
using ByteSpan = std::span<const u8>;
using MutByteSpan = std::span<u8>;

// ---- 线格式常量 -------------------------------------------------------------
// 单个 UDP 包的最大载荷。1200 + 32 字节包头 = 1232，远低于 1500 MTU，
// 为 IP/UDP 头和隧道封装留足余量。
inline constexpr std::size_t kMaxPacketPayload = 1200;
inline constexpr std::size_t kPacketHeaderSize = 32;
inline constexpr std::size_t kMaxPacketSize = kPacketHeaderSize + kMaxPacketPayload;

// 协议魔数 'D''U''O''1'（小端）
inline constexpr u32 kProtocolMagic = 0x314F5544u;
inline constexpr u16 kProtocolVersion = 1;

// 默认端口
inline constexpr u16 kDefaultVideoPort = 50000;
inline constexpr u16 kDefaultControlPort = 50001;

}  // namespace duo
