// 抖动缓冲接收路径的微基准：量化 FEC 恢复的开销。
//
// 模拟真实场景：每帧 40 个数据包 + FEC 校验包，按序插入，
// 其中每 16 包丢 1 个（FEC 组大小 16，正好每组可恢复）。
// 测量「把一整帧的所有包 insert 进抖动缓冲」的耗时。
//
// 这个基准专门针对 FEC 恢复路径：旧实现在每个包到达时都全量
// 扫描整帧做 FEC 运算，复杂度 O(n²)。现在 insert 只做 O(1) 计数，
// 恢复发生在 service()（NACK 期限之后），所以两个阶段都要计时。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/net/fec.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/packetizer.h"

namespace {

duo::Bytes make_frame(std::size_t size, unsigned seed) {
  duo::Bytes data(size);
  std::mt19937 rng(seed);
  for (auto& byte : data) {
    byte = static_cast<duo::u8>(rng() & 0xFF);
  }
  return data;
}

// 跑 iterations 帧，返回总耗时（毫秒）。
double run_case(std::size_t payload_per_packet, std::size_t packets_per_frame,
                std::size_t group_size, std::size_t drop_every, int iterations,
                duo::u64& recovered_out) {
  const duo::Bytes frame = make_frame(payload_per_packet * packets_per_frame, 12345);

  duo::FrameMeta meta{};
  meta.frame_id = 0;
  meta.key = true;

  const duo::Packetizer packetizer(payload_per_packet);

  // 预先算好所有帧的包，避免把打包开销算进去
  std::vector<std::vector<duo::Packet>> prepared;
  prepared.reserve(static_cast<std::size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    meta.frame_id = static_cast<duo::u32>(i);
    auto packets = packetizer.packetize(meta, frame);
    auto parity = duo::FecCodec::encode_frame(packets, group_size);

    std::vector<duo::Packet> wire;
    wire.reserve(packets.size() + parity.size());
    // 模拟丢包：每 drop_every 个数据包丢一个
    for (const auto& packet : packets) {
      if (drop_every != 0 && packet.header.pkt_index % drop_every == 0 &&
          packet.header.pkt_index != 0) {
        continue;
      }
      wire.push_back(packet);
    }
    for (const auto& check : parity) {
      wire.push_back(check);
    }
    prepared.push_back(std::move(wire));
  }

  const duo::u64 begin = duo::Clock::now_ns();
  duo::u64 recovered = 0;
  duo::u64 assembled = 0;
  for (int i = 0; i < iterations; ++i) {
    duo::JitterBuffer jitter(64, 0);  // NACK 延迟 0：service 立即做恢复
    for (const auto& packet : prepared[static_cast<std::size_t>(i)]) {
      jitter.insert(packet);
    }
    jitter.service(duo::Clock::now_ns() + 1'000'000'000ull);
    duo::AssembledFrame out;
    while (jitter.try_pop(out)) {
      ++assembled;
    }
    recovered += jitter.stats().packets_recovered;
  }
  // 恢复数为 0 说明基准跑了空转，比出来的耗时没有意义。
  if (recovered == 0 && drop_every != 0) {
    std::printf("  !! 未发生任何 FEC 恢复，基准无效\n");
  }
  if (assembled != static_cast<duo::u64>(iterations)) {
    std::printf("  !! 只组装出 %llu/%d 帧，恢复不完整\n",
                static_cast<unsigned long long>(assembled), iterations);
  }
  recovered_out = recovered;
  return duo::Clock::to_ms(duo::Clock::now_ns() - begin);
}

}  // namespace

int main() {
  constexpr int kIterations = 200;

  std::printf("抖动缓冲接收路径基准 (%d 帧/组)\n", kIterations);
  std::printf("%-38s %10s %14s %10s\n", "场景", "总耗时(ms)", "每帧(us)", "FEC恢复");
  struct Case {
    const char* name;
    std::size_t payload;
    std::size_t packets;
    std::size_t group;
    std::size_t drop_every;
  };

  const Case cases[] = {
      {"1200B x 40 包/帧, 每16包丢1", 1200, 40, 16, 16},
      {"1200B x 40 包/帧, 无丢包", 1200, 40, 16, 0},
      {"1200B x 200 包/帧, 每16包丢1", 1200, 200, 16, 16},
      {"1200B x 40 包/帧, 关闭FEC", 1200, 40, 0, 0},
  };

  for (const Case& c : cases) {
    duo::u64 recovered = 0;
    const double ms = run_case(c.payload, c.packets, c.group == 0 ? 0 : c.group, c.drop_every,
                               kIterations, recovered);
    std::printf("%-38s %10.2f %14.1f %10llu\n", c.name, ms, ms * 1000.0 / kIterations,
                static_cast<unsigned long long>(recovered));
  }

  return 0;
}
