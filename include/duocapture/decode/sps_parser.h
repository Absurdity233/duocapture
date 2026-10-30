#pragma once

#include <cstdint>

#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

namespace duo {

// 从 Annex-B 的 SPS NAL 里解析出编码分辨率。
//
// 为什么需要它：MF 的解码 MFT 在输入类型里没有 FRAME_SIZE 时会先
// 返回 STREAM_CHANGE，随后的重协商在同步喂帧模式下会失败
// （ProcessInput 返回 0xC00D36B5）。把 SPS 里的真实尺寸填进输入类型
// 可以完全避开这条路径。
//
// 只解析到分辨率，不做完整的 SPS 语法校验——够用即可。
struct SpsInfo {
  u32 width = 0;
  u32 height = 0;
  bool valid = false;
};

// 从一段 Annex-B 数据里找 SPS 并解析尺寸。
SpsInfo parse_sps_dimensions(ByteSpan annex_b, Codec codec);

// 从裸 SPS 负载（不含起始码和 NAL 头）解析尺寸。
SpsInfo parse_h264_sps(ByteSpan rbsp);
SpsInfo parse_hevc_sps(ByteSpan rbsp);

// 去掉防竞争字节 0x000003 → 0x0000。
Bytes remove_emulation_prevention(ByteSpan data);

}  // namespace duo
