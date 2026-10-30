// 控制通道与 NACK 的回归测试。
//
// NACK 最容易坏在"压缩区间"这一步：区间算错会导致重传请求指向错误的包，
// 表现为「收到了重传但仍然花屏」，而且很难从日志上看出来。
#include <vector>

#include "duocapture/common/types.h"
#include "duocapture/net/control_channel.h"
#include "testfw.h"

DUO_TEST(nack_compacts_contiguous_runs) {
  // 同一帧里 3 个连续丢包应该压成一个区间
  const std::vector<std::pair<duo::u32, duo::u16>> missing = {{7, 10}, {7, 11}, {7, 12}};
  const auto ranges = duo::compact_nacks(missing);
  DUO_CHECK_EQ(ranges.size(), static_cast<std::size_t>(1));
  if (!ranges.empty()) {
    DUO_CHECK_EQ(ranges[0].frame_id, 7u);
    DUO_CHECK_EQ(ranges[0].begin, static_cast<duo::u16>(10));
    DUO_CHECK_EQ(ranges[0].end, static_cast<duo::u16>(12));
  }
}

DUO_TEST(nack_splits_disjoint_runs) {
  // 10,11 连续；15 单独；17,18 连续 → 三个区间
  const std::vector<std::pair<duo::u32, duo::u16>> missing = {
      {7, 10}, {7, 11}, {7, 15}, {7, 17}, {7, 18}};
  const auto ranges = duo::compact_nacks(missing);
  DUO_CHECK_EQ(ranges.size(), static_cast<std::size_t>(3));
  if (ranges.size() == 3) {
    DUO_CHECK_EQ(ranges[0].begin, static_cast<duo::u16>(10));
    DUO_CHECK_EQ(ranges[0].end, static_cast<duo::u16>(11));
    DUO_CHECK_EQ(ranges[1].begin, static_cast<duo::u16>(15));
    DUO_CHECK_EQ(ranges[1].end, static_cast<duo::u16>(15));
    DUO_CHECK_EQ(ranges[2].begin, static_cast<duo::u16>(17));
    DUO_CHECK_EQ(ranges[2].end, static_cast<duo::u16>(18));
  }
}

DUO_TEST(nack_splits_across_frames) {
  // 不同帧的包绝不能并进同一区间
  const std::vector<std::pair<duo::u32, duo::u16>> missing = {{7, 10}, {8, 11}};
  const auto ranges = duo::compact_nacks(missing);
  DUO_CHECK_EQ(ranges.size(), static_cast<std::size_t>(2));
  if (ranges.size() == 2) {
    DUO_CHECK_EQ(ranges[0].frame_id, 7u);
    DUO_CHECK_EQ(ranges[1].frame_id, 8u);
  }
}

DUO_TEST(nack_handles_unsorted_input) {
  // 乱序输入也要压成正确的区间
  const std::vector<std::pair<duo::u32, duo::u16>> missing = {{7, 12}, {7, 10}, {7, 11}};
  const auto ranges = duo::compact_nacks(missing);
  DUO_CHECK_EQ(ranges.size(), static_cast<std::size_t>(1));
  if (!ranges.empty()) {
    DUO_CHECK_EQ(ranges[0].begin, static_cast<duo::u16>(10));
    DUO_CHECK_EQ(ranges[0].end, static_cast<duo::u16>(12));
  }
}

DUO_TEST(nack_empty_input) {
  const std::vector<std::pair<duo::u32, duo::u16>> missing;
  DUO_CHECK(duo::compact_nacks(missing).empty());
}

DUO_TEST(control_roundtrip) {
  const std::vector<duo::NackRange> ranges = {{3, 5, 9}, {4, 0, 2}};
  const duo::Bytes msg = duo::serialize_control(
      duo::ControlType::Nack,
      duo::ByteSpan(reinterpret_cast<const duo::u8*>(ranges.data()),
                    ranges.size() * sizeof(duo::NackRange)));

  duo::ControlHeader header{};
  duo::ByteSpan payload;
  DUO_CHECK(duo::parse_control(duo::ByteSpan(msg.data(), msg.size()), header, payload));
  DUO_CHECK_EQ(header.type, static_cast<duo::u8>(duo::ControlType::Nack));
  DUO_CHECK_EQ(header.count, static_cast<duo::u8>(2));
  DUO_CHECK_EQ(payload.size(), sizeof(duo::NackRange) * 2);

  const auto* parsed = reinterpret_cast<const duo::NackRange*>(payload.data());
  DUO_CHECK_EQ(parsed[0].frame_id, 3u);
  DUO_CHECK_EQ(parsed[0].begin, static_cast<duo::u16>(5));
  DUO_CHECK_EQ(parsed[0].end, static_cast<duo::u16>(9));
  DUO_CHECK_EQ(parsed[1].frame_id, 4u);
}

DUO_TEST(control_rejects_bad_magic) {
  duo::Bytes junk(32, 0);
  duo::ControlHeader header{};
  duo::ByteSpan payload;
  DUO_CHECK(!duo::parse_control(duo::ByteSpan(junk.data(), junk.size()), header, payload));
}

DUO_TEST(control_idr_request_has_no_payload) {
  const duo::Bytes msg = duo::serialize_control(duo::ControlType::RequestIdr, {});
  duo::ControlHeader header{};
  duo::ByteSpan payload;
  DUO_CHECK(duo::parse_control(duo::ByteSpan(msg.data(), msg.size()), header, payload));
  DUO_CHECK_EQ(header.type, static_cast<duo::u8>(duo::ControlType::RequestIdr));
  DUO_CHECK(payload.empty());
}
