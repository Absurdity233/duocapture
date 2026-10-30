#include "duocapture/pipeline/selftest.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/net/fec.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/pacer.h"
#include "duocapture/net/packetizer.h"

namespace duo {
namespace {

bool check(bool condition, const char* what, bool verbose) {
  if (verbose) {
    std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
  }
  return condition;
}

}  // namespace

bool run_net_selftest(bool verbose) {
  bool pass = true;

  // 1. 构造一帧约 200 KB 的伪随机数据（约 171 个包）。
  constexpr std::size_t kFrameSize = 200 * 1024;
  Bytes source(kFrameSize);
  std::mt19937 rng(0x5EED);
  for (auto& byte : source) {
    byte = static_cast<u8>(rng() & 0xFF);
  }

  FrameMeta meta{};
  meta.frame_id = 7;
  meta.capture_ts_ns = Clock::now_ns();
  meta.key = true;
  meta.width = 2560;
  meta.height = 1440;

  // 2. 打包
  const Packetizer packetizer;
  std::vector<Packet> packets = packetizer.packetize(meta, source);
  pass &= check(!packets.empty(), "packetize produced packets", verbose);

  // 3. FEC：每 16 个数据包 1 个校验包
  std::vector<Packet> parity = FecCodec::encode_frame(packets, 16);
  pass &= check(!parity.empty(), "fec produced parity packets", verbose);

  // 4. 模拟链路：打乱顺序 + 固定丢 3 个包（分属不同 FEC 组）
  std::vector<Packet> wire;
  wire.reserve(packets.size() + parity.size());
  wire.insert(wire.end(), packets.begin(), packets.end());
  wire.insert(wire.end(), parity.begin(), parity.end());
  std::shuffle(wire.begin(), wire.end(), rng);

  std::vector<Packet> received;
  received.reserve(wire.size());
  int dropped = 0;
  for (const Packet& packet : wire) {
    const u16 index = packet.header.pkt_index;
    const bool drop = !packet.is_parity() && (index == 0 || index == 20 || index == 40);
    if (drop) {
      ++dropped;
      continue;
    }
    received.push_back(packet);
  }
  pass &= check(dropped == 3, "simulated 3 packet drops", verbose);

  // 5. 抖动缓冲重组
  JitterBuffer jitter;
  for (const Packet& packet : received) {
    jitter.insert(packet);
  }

  // FEC 恢复发生在 NACK 期限之后（避免把「只是晚到」当成丢包），
  // 这里模拟时间已经走过期限。
  jitter.service(Clock::now_ns() + 50'000'000ull);

  AssembledFrame frame;
  const bool got = jitter.try_pop(frame);
  pass &= check(got, "jitter buffer assembled the frame", verbose);
  if (got) {
    pass &= check(frame.data.size() == source.size(), "assembled size matches", verbose);
    pass &= check(frame.data == source, "assembled content matches (FEC recovered all drops)",
                  verbose);
    pass &= check(frame.frame_id == meta.frame_id, "frame id matches", verbose);
    pass &= check(frame.key, "key frame flag preserved", verbose);
  }

  if (verbose) {
    std::printf("  packets=%zu parity=%zu recovered=%llu assembled=%llu\n", packets.size(),
                parity.size(), static_cast<unsigned long long>(jitter.stats().packets_recovered),
                static_cast<unsigned long long>(jitter.stats().frames_assembled));
  }

  // 6. 限速器：整帧入队后，单次 poll 不应把整帧都放行。
  // 用较小的 burst（4 KB）确保节流行为可观测，而不是依赖默认 burst 大小。
  Pacer pacer(50ull * 1000 * 1000, 4 * 1024);  // 50 MB/s, burst 4 KB
  for (const Packet& packet : packets) {
    pacer.enqueue(packet);
  }
  std::vector<Packet> out(packets.size());
  const std::size_t released = pacer.poll(Clock::now_ns(), out.data(), out.size());
  pass &= check(released < packets.size(), "pacer throttles burst instead of dumping the frame",
                verbose);
  pass &= check(released > 0, "pacer releases at least one packet", verbose);

  return pass;
}

}  // namespace duo
