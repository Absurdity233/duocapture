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

DUO_TEST(pacer_throttles) {
  duo::Pacer pacer(10ull * 1000 * 1000);  // 10 MB/s

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
}

DUO_TEST(selftest_passes) { DUO_CHECK(duo::run_net_selftest(false)); }
