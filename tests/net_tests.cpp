#include <algorithm>
#include <random>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/ring_buffer.h"
#include "duocapture/net/fec.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/pacer.h"
#include "duocapture/net/packet.h"
#include "duocapture/net/packetizer.h"
#include "duocapture/pipeline/selftest.h"
#include "testfw.h"

namespace {

duo::Bytes make_payload(std::size_t size, unsigned seed) {
  duo::Bytes data(size);
  std::mt19937 rng(seed);
  for (auto& byte : data) {
    byte = static_cast<duo::u8>(rng() & 0xFF);
  }
  return data;
}

}  // namespace

DUO_TEST(ring_buffer_push_pop) {
  duo::SpscRing<int, 8> ring;
  DUO_CHECK(ring.empty());

  for (int i = 0; i < 7; ++i) {
    DUO_CHECK(ring.try_push(i));
  }
  DUO_CHECK(!ring.try_push(7));  // 满
  DUO_CHECK_EQ(ring.size(), static_cast<std::size_t>(7));

  for (int i = 0; i < 7; ++i) {
    int value = -1;
    DUO_CHECK(ring.try_pop(value));
    DUO_CHECK_EQ(value, i);
  }
  DUO_CHECK(ring.empty());
  int value = 0;
  DUO_CHECK(!ring.try_pop(value));
}

DUO_TEST(packet_serialize_parse_roundtrip) {
  duo::PacketHeader header{};
  header.magic = duo::kProtocolMagic;
  header.version = duo::kProtocolVersion;
  header.flags = duo::kFlagKeyFrame;
  header.frame_id = 42;
  header.pkt_index = 3;
  header.pkt_count = 10;
  header.capture_ts_ns = 123456789ull;
  header.payload_len = 5;

  const duo::Bytes payload = {1, 2, 3, 4, 5};
  const duo::Bytes wire = duo::serialize(header, payload);

  duo::PacketHeader parsed{};
  duo::ByteSpan parsed_payload;
  DUO_CHECK(duo::parse(duo::ByteSpan(wire.data(), wire.size()), parsed, parsed_payload));
  DUO_CHECK_EQ(parsed.frame_id, 42u);
  DUO_CHECK_EQ(parsed.pkt_index, static_cast<duo::u16>(3));
  DUO_CHECK_EQ(parsed_payload.size(), static_cast<std::size_t>(5));
  DUO_CHECK_EQ(parsed_payload[4], static_cast<duo::u8>(5));
}

DUO_TEST(packet_parse_rejects_bad_magic) {
  duo::Bytes wire(duo::kPacketHeaderSize, 0);
  duo::PacketHeader parsed{};
  duo::ByteSpan payload;
  DUO_CHECK(!duo::parse(duo::ByteSpan(wire.data(), wire.size()), parsed, payload));
}

DUO_TEST(packetizer_splits_frame) {
  const duo::Bytes frame = make_payload(5000, 1);
  duo::FrameMeta meta{};
  meta.frame_id = 1;
  meta.key = true;

  const duo::Packetizer packetizer;
  const auto packets = packetizer.packetize(meta, frame);

  DUO_CHECK_EQ(packets.size(), static_cast<std::size_t>(5));  // 5000 / 1200 -> 5
  DUO_CHECK(packets.front().header.flags & duo::kFlagFrameStart);
  DUO_CHECK(packets.back().header.flags & duo::kFlagFrameEnd);
  DUO_CHECK(packets.front().is_key());

  std::size_t total = 0;
  for (const auto& packet : packets) {
    total += packet.payload.size();
  }
  DUO_CHECK_EQ(total, frame.size());
}

DUO_TEST(fec_recovers_single_loss) {
  const duo::Bytes frame = make_payload(200 * 1024, 2);
  duo::FrameMeta meta{};
  meta.frame_id = 9;

  const duo::Packetizer packetizer;
  auto packets = packetizer.packetize(meta, frame);
  auto parity = duo::FecCodec::encode_frame(packets, 16);

  // 丢掉第 5 个数据包
  std::vector<duo::Packet> working;
  for (const auto& packet : packets) {
    if (packet.header.pkt_index == 5) {
      continue;
    }
    working.push_back(packet);
  }
  for (const auto& check : parity) {
    working.push_back(check);
  }

  DUO_CHECK(duo::FecCodec::recover_one(working));

  const duo::Packet* recovered = nullptr;
  for (const auto& packet : working) {
    if (!packet.is_parity() && packet.header.pkt_index == 5) {
      recovered = &packet;
    }
  }
  DUO_CHECK(recovered != nullptr);
  if (recovered != nullptr) {
    DUO_CHECK_EQ(recovered->payload.size(), packets[5].payload.size());
    DUO_CHECK(recovered->payload == packets[5].payload);
  }
}

DUO_TEST(jitter_buffer_reorders) {
  const duo::Bytes frame = make_payload(6000, 3);
  duo::FrameMeta meta{};
  meta.frame_id = 11;

  const duo::Packetizer packetizer;
  auto packets = packetizer.packetize(meta, frame);

  std::mt19937 rng(7);
  std::shuffle(packets.begin(), packets.end(), rng);

  duo::JitterBuffer jitter;
  for (const auto& packet : packets) {
    jitter.insert(packet);
  }

  duo::AssembledFrame out;
  DUO_CHECK(jitter.try_pop(out));
  DUO_CHECK_EQ(out.frame_id, 11u);
  DUO_CHECK(out.data == frame);
}

DUO_TEST(jitter_buffer_nacks_missing) {
  const duo::Bytes frame = make_payload(6000, 4);
  duo::FrameMeta meta{};
  meta.frame_id = 12;

  const duo::Packetizer packetizer;
  auto packets = packetizer.packetize(meta, frame);

  duo::JitterBuffer jitter(16, 0);  // NACK 延迟 0 ms
  for (const auto& packet : packets) {
    if (packet.header.pkt_index == 2) {
      continue;  // 丢一个
    }
    jitter.insert(packet);
  }

  const auto missing = jitter.collect_nacks(duo::Clock::now_ns() + 10'000'000ull);
  DUO_CHECK_EQ(missing.size(), static_cast<std::size_t>(1));
  if (!missing.empty()) {
    DUO_CHECK_EQ(missing.front().first, 12u);
    DUO_CHECK_EQ(missing.front().second, static_cast<duo::u16>(2));
  }
}

DUO_TEST(jitter_buffer_recovers_tail_group_after_timeout) {
  // 帧尾那组通常不满 group_size，而且尾包载荷常常超过 255 字节。
  // 旧实现有两处会在这里出错：组基址用 group*group_size 推算（尾组错位），
  // 尾包长度只存低 8 位（800 字节被截成 32）。
  const duo::Bytes frame = make_payload(200 * 1024, 5);  // 171 包，尾组 11 个
  duo::FrameMeta meta{};
  meta.frame_id = 21;

  const duo::Packetizer packetizer;
  auto packets = packetizer.packetize(meta, frame);
  auto parity = duo::FecCodec::encode_frame(packets, 16);

  const duo::u16 last_index = static_cast<duo::u16>(packets.size() - 1);
  DUO_CHECK(packets.back().payload.size() > 255);

  duo::JitterBuffer jitter(16, 0);  // NACK 延迟 0，立即允许 FEC
  for (const auto& packet : packets) {
    if (packet.header.pkt_index == last_index) {
      continue;  // 丢掉尾包
    }
    jitter.insert(packet);
  }
  for (const auto& check : parity) {
    jitter.insert(check);
  }
  jitter.service(duo::Clock::now_ns() + 50'000'000ull);

  duo::AssembledFrame out;
  DUO_CHECK(jitter.try_pop(out));
  DUO_CHECK_EQ(out.data.size(), frame.size());
  DUO_CHECK(out.data == frame);
}

DUO_TEST(jitter_buffer_does_not_recover_before_timeout) {
  // 乱序链路上，缺的那个包通常只是还在路上。包一到就恢复会把它们
  // 全部"恢复"一遍，白烧校验包还算小事，恢复包被真包顶掉后帧仍然
  // 完整，问题被完全掩盖。这里要求：没过 NACK 期限时一次都不恢复。
  const duo::Bytes frame = make_payload(200 * 1024, 6);
  duo::FrameMeta meta{};
  meta.frame_id = 22;

  const duo::Packetizer packetizer;
  auto packets = packetizer.packetize(meta, frame);
  auto parity = duo::FecCodec::encode_frame(packets, 16);

  duo::JitterBuffer jitter(16, 1000);  // 1 秒后才允许恢复
  std::mt19937 rng(11);
  std::vector<duo::Packet> wire(packets);
  wire.insert(wire.end(), parity.begin(), parity.end());
  std::shuffle(wire.begin(), wire.end(), rng);

  for (const auto& packet : wire) {
    if (!packet.is_parity() && packet.header.pkt_index % 16 == 3) {
      continue;  // 每组丢 1 个
    }
    jitter.insert(packet);
  }
  jitter.service(duo::Clock::now_ns());
  DUO_CHECK_EQ(jitter.stats().packets_recovered, 0ull);

  jitter.service(duo::Clock::now_ns() + 2'000'000'000ull);
  DUO_CHECK_EQ(jitter.stats().packets_recovered, static_cast<duo::u64>(parity.size()));
}

DUO_TEST(jitter_buffer_fec_after_nack_retransmit) {
  // 先丢 2 个包（XOR 救不回来），NACK 补回 1 个之后只剩 1 个缺口，
  // 此时 FEC 才应该生效。恢复状态不能在"缺 2 个"时被永久放弃。
  const duo::Bytes frame = make_payload(6000, 7);  // 5 包，单组
  duo::FrameMeta meta{};
  meta.frame_id = 23;

  const duo::Packetizer packetizer;
  auto packets = packetizer.packetize(meta, frame);
  auto parity = duo::FecCodec::encode_frame(packets, 16);
  DUO_CHECK_EQ(parity.size(), static_cast<std::size_t>(1));

  duo::JitterBuffer jitter(16, 0);
  for (const auto& packet : packets) {
    if (packet.header.pkt_index == 1 || packet.header.pkt_index == 3) {
      continue;
    }
    jitter.insert(packet);
  }
  jitter.insert(parity.front());

  jitter.service(duo::Clock::now_ns() + 50'000'000ull);
  DUO_CHECK_EQ(jitter.stats().packets_recovered, 0ull);  // 缺 2 个，救不了

  duo::AssembledFrame out;
  DUO_CHECK(!jitter.try_pop(out));

  // NACK 重传补回 1 号包
  jitter.insert(packets[1]);
  jitter.service(duo::Clock::now_ns() + 100'000'000ull);

  DUO_CHECK(jitter.try_pop(out));
  DUO_CHECK(out.data == frame);
  DUO_CHECK_EQ(jitter.stats().packets_recovered, 1ull);
}

DUO_TEST(pacer_throttles) {
  // 1 MB/s，100 个满载包（约 123 KB）远超 burst 上限，必须分多次才放完。
  duo::Pacer pacer(1000ull * 1000, 8 * 1024);  // 1 MB/s, burst 8 KB

  duo::Packet packet;
  packet.header.payload_len = duo::kMaxPacketPayload;
  packet.payload.assign(duo::kMaxPacketPayload, 0);

  for (int i = 0; i < 100; ++i) {
    pacer.enqueue(packet);
  }
  DUO_CHECK_EQ(pacer.queued(), static_cast<std::size_t>(100));

  std::vector<duo::Packet> out(100);
  const std::size_t released = pacer.poll(duo::Clock::now_ns(), out.data(), out.size());
  DUO_CHECK(released > 0);
  DUO_CHECK(released < 100);
  DUO_CHECK_EQ(pacer.queued(), static_cast<std::size_t>(100 - released));
}

DUO_TEST(selftest_passes) { DUO_CHECK(duo::run_net_selftest(false)); }
