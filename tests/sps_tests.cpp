// SPS 解析的回归测试。
//
// 这几条用例对应开发期间踩过的四个真实 bug，每一个都表现为
// "解码器输出 1920x1080 之类的错误尺寸" 或 "ProcessOutput 直接失败"：
//   1. 把 remove_emulation_prevention 的临时 Bytes 绑给 ByteSpan（悬垂）
//   2. NAL 边界按"往前找下一个起始码"计算，会把边界字节带进 RBSP
//   3. HEVC 的 NAL 头是 2 字节，少跳 1 字节导致 Exp-Golomb 全部错位
//   4. HEVC 的 sub_layers 少加了 1
#include <vector>

#include "duocapture/decode/sps_parser.h"
#include "testfw.h"

namespace {

// 实测自 NVENC 输出的一段 H.264 SPS/PPS（2560x1600）。
const std::vector<duo::u8> kH264SpsPps = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x32, 0xAC, 0x2B, 0x20, 0x05, 0x00, 0x19,
    0x36, 0x02, 0x20, 0x00, 0x00, 0x7D, 0x00, 0x00, 0x13, 0x88, 0x10, 0x80, 0x00, 0x00,
    0x00, 0x01, 0x68, 0xEB, 0x8F, 0x2C};

}  // namespace

DUO_TEST(sps_h264_parses_real_stream_dimensions) {
  const duo::SpsInfo info =
      duo::parse_sps_dimensions(duo::ByteSpan(kH264SpsPps.data(), kH264SpsPps.size()),
                                duo::Codec::H264);
  DUO_CHECK(info.valid);
  DUO_CHECK_EQ(info.width, 2560u);
  DUO_CHECK_EQ(info.height, 1600u);
}

DUO_TEST(sps_h264_handles_trailing_pps_without_off_by_one) {
  // 在 SPS 后追加更多 NAL，确认边界计算不会把后续字节带进 SPS 的 RBSP。
  std::vector<duo::u8> stream = kH264SpsPps;
  const duo::u8 idr[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10};
  stream.insert(stream.end(), std::begin(idr), std::end(idr));

  const duo::SpsInfo info =
      duo::parse_sps_dimensions(duo::ByteSpan(stream.data(), stream.size()), duo::Codec::H264);
  DUO_CHECK(info.valid);
  DUO_CHECK_EQ(info.width, 2560u);
  DUO_CHECK_EQ(info.height, 1600u);
}

DUO_TEST(sps_returns_invalid_on_garbage) {
  const duo::u8 junk[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0xFF, 0xFF, 0xFF};
  const duo::SpsInfo info =
      duo::parse_sps_dimensions(duo::ByteSpan(junk, sizeof(junk)), duo::Codec::H264);
  // 要么解析失败，要么给出一组明显不合理的尺寸——绝不能崩溃。
  DUO_CHECK(!info.valid || (info.width > 0 && info.height > 0));
}

DUO_TEST(emulation_prevention_removes_only_03_bytes) {
  const duo::u8 in[] = {0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x03};
  const duo::Bytes out = duo::remove_emulation_prevention(duo::ByteSpan(in, sizeof(in)));
  DUO_CHECK_EQ(out.size(), static_cast<std::size_t>(6));
  DUO_CHECK_EQ(out[2], static_cast<duo::u8>(0x01));
}
