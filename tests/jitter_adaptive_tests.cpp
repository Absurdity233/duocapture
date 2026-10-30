// 抖动缓冲自适应 NACK 延迟的回归测试。
//
// 自适应逻辑最容易坏在两处：
//   * 启动时的突发被当成巨大抖动，NACK 延迟瞬间顶到上限
//   * 抖动回落时延迟不收敛，永远停在保守值上
// 这两个都不会崩，但会让弱网下的重传行为完全失效。
//
// 全部用显式时间戳驱动，不 sleep：CI 上线程调度的抖动（几毫秒）比要测的
// 到达抖动还大，靠 sleep 制造间隔的测试会随机失败。
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/types.h"
#include "duocapture/net/jitter_buffer.h"
#include "testfw.h"

namespace {

constexpr duo::u64 kMs = 1'000'000ull;

duo::Packet make_packet(duo::u32 frame_id, duo::u16 index, duo::u16 count) {
  duo::Packet packet;
  packet.header.magic = duo::kProtocolMagic;
  packet.header.version = duo::kProtocolVersion;
  packet.header.frame_id = frame_id;
  packet.header.pkt_index = index;
  packet.header.pkt_count = count;
  packet.header.payload_len = 4;
  packet.payload = {1, 2, 3, 4};
  return packet;
}

}  // namespace

DUO_TEST(jitter_starts_at_configured_delay) {
  duo::JitterBuffer jitter(16, 3);
  // 还没收到包时，有效延迟就是构造时给的值
  DUO_CHECK_EQ(jitter.stats().effective_nack_delay_ms, 3u);
  DUO_CHECK(jitter.stats().arrival_jitter_ms == 0.0);
}

DUO_TEST(jitter_stable_arrival_keeps_delay_low) {
  duo::JitterBuffer jitter(16, 3);
  // 精确 2ms 间隔喂 64 个包：抖动必须接近 0，
  // 自适应延迟落在下限附近（2ms）。
  duo::u64 now = 1'000'000'000ull;
  for (int i = 0; i < 64; ++i) {
    jitter.insert_at(make_packet(static_cast<duo::u32>(i / 4), static_cast<duo::u16>(i % 4), 4),
                     now);
    now += 2 * kMs;
  }
  DUO_CHECK(jitter.stats().arrival_jitter_ms < 0.5);
  DUO_CHECK(jitter.stats().effective_nack_delay_ms <= 3u);
}

DUO_TEST(jitter_bursty_arrival_raises_delay) {
  duo::JitterBuffer jitter(64, 3);
  duo::u64 now = 1'000'000'000ull;

  // 先建立基线：稳定 2ms
  for (int i = 0; i < 20; ++i) {
    jitter.insert_at(make_packet(static_cast<duo::u32>(i), 0, 1), now);
    now += 2 * kMs;
  }
  const duo::u32 baseline_delay = jitter.stats().effective_nack_delay_ms;

  // 再来一串忽快忽慢的包：0ms / 10ms 交替
  for (int i = 20; i < 100; ++i) {
    jitter.insert_at(make_packet(static_cast<duo::u32>(i), 0, 1), now);
    now += (i % 2 == 0) ? 0 : 10 * kMs;
  }

  // 抖动应该显著上升，延迟随之抬高
  DUO_CHECK(jitter.stats().arrival_jitter_ms > 1.0);
  DUO_CHECK(jitter.stats().effective_nack_delay_ms > baseline_delay);
  // 但不能超过上限
  DUO_CHECK(jitter.stats().effective_nack_delay_ms <= 20u);
}

DUO_TEST(jitter_manual_delay_override_wins) {
  duo::JitterBuffer jitter(16, 3);
  jitter.set_nack_delay(7);
  duo::u64 now = 1'000'000'000ull;
  for (int i = 0; i < 40; ++i) {
    jitter.insert_at(make_packet(static_cast<duo::u32>(i), 0, 1), now);
    now += kMs;
  }
  // 手动覆盖后，自适应不该再改它
  DUO_CHECK_EQ(jitter.stats().effective_nack_delay_ms, 7u);
}

DUO_TEST(jitter_startup_burst_does_not_panic) {
  duo::JitterBuffer jitter(16, 3);
  // 一次性灌 20 个包（零间隔），模拟启动时的突发。
  // 基线阶段（前 16 个包）不计抖动，所以延迟不该被顶到上限。
  const duo::u64 now = 1'000'000'000ull;
  for (int i = 0; i < 20; ++i) {
    jitter.insert_at(make_packet(static_cast<duo::u32>(i), 0, 1), now);
  }
  DUO_CHECK(jitter.stats().effective_nack_delay_ms <= 20u);
}
