// M3/M4 的回归测试：分析 tap 的丢帧语义、滚动统计的百分位。
//
// 这两块最容易在重构时被"顺手优化坏"：
//   * 分析 tap 一旦改成"不丢帧"，就会阻塞解码线程，呈现开始卡
//   * 百分位算错不会崩，但遥测数字会静默失真
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "duocapture/common/types.h"
#include "duocapture/pipeline/analysis_tap.h"
#include "testfw.h"

namespace {

struct Counter {
  std::atomic<duo::u64> calls{0};
  std::atomic<int> delay_ms{0};
};

void counting_callback(void* user, const duo::DecodedFrame&) {
  auto* counter = static_cast<Counter*>(user);
  counter->calls.fetch_add(1, std::memory_order_relaxed);
  const int d = counter->delay_ms.load(std::memory_order_relaxed);
  if (d > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(d));
  }
}

duo::DecodedFrame make_frame() {
  duo::DecodedFrame frame{};
  // 只用一个非空指针占位；tap 不 dereference 它，回调里也不碰。
  static int dummy = 0;
  frame.texture = &dummy;
  frame.width = 640;
  frame.height = 480;
  return frame;
}

}  // namespace

DUO_TEST(analysis_tap_delivers_frames) {
  Counter counter;
  duo::AnalysisTap tap;
  DUO_CHECK(duo::ok(tap.start(&counting_callback, &counter)));

  for (int i = 0; i < 10; ++i) {
    tap.submit(make_frame());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  tap.stop();

  DUO_CHECK(counter.calls.load() >= 8);
  DUO_CHECK(tap.frames_submitted() == 10);
}

DUO_TEST(analysis_tap_drops_when_busy_but_never_blocks) {
  Counter counter;
  counter.delay_ms.store(50);  // 每帧 50ms，处理能力 20fps
  duo::AnalysisTap tap;
  DUO_CHECK(duo::ok(tap.start(&counting_callback, &counter)));

  // 快速提交 40 帧，远快于处理速度。
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 40; ++i) {
    tap.submit(make_frame());
  }
  const auto elapsed = std::chrono::steady_clock::now() - begin;

  // 关键断言：submit 必须立即返回。如果这里阻塞了，说明队列在背压，
  // 解码线程就会被拖死，呈现开始掉帧。
  const auto submit_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  DUO_CHECK(submit_ms < 20);

  tap.stop();

  // 分析次数远少于提交次数 —— 证明丢帧语义生效
  DUO_CHECK(tap.frames_submitted() == 40);
  DUO_CHECK(tap.frames_analyzed() < 40);
  DUO_CHECK(tap.frames_dropped() > 0);
}

DUO_TEST(analysis_tap_stop_is_idempotent) {
  Counter counter;
  duo::AnalysisTap tap;
  tap.stop();  // 未启动时调用不应崩溃
  DUO_CHECK(duo::ok(tap.start(&counting_callback, &counter)));
  tap.stop();
  tap.stop();  // 重复 stop 也应安全
  DUO_CHECK(true);
}

DUO_TEST(rolling_stats_percentiles) {
  duo::RollingStats stats(100);
  for (int i = 1; i <= 100; ++i) {
    stats.add(static_cast<double>(i));
  }
  DUO_CHECK(stats.count() == 100);
  // 100 个值 1..100，中位数应接近 50，p99 接近 99
  const double p50 = stats.percentile(0.5);
  const double p99 = stats.percentile(0.99);
  DUO_CHECK(p50 >= 49.0 && p50 <= 51.0);
  DUO_CHECK(p99 >= 98.0 && p99 <= 100.0);
  DUO_CHECK(stats.mean() > 49.0 && stats.mean() < 52.0);
}

DUO_TEST(rolling_stats_window_wraps) {
  duo::RollingStats stats(10);
  // 灌 20 个值，窗口只保留最近 10 个（11..20）
  for (int i = 1; i <= 20; ++i) {
    stats.add(static_cast<double>(i));
  }
  DUO_CHECK(stats.count() == 10);
  const double mean = stats.mean();
  DUO_CHECK(mean >= 15.0 && mean <= 16.0);
}

DUO_TEST(rolling_stats_empty_is_safe) {
  duo::RollingStats stats(10);
  DUO_CHECK(stats.mean() == 0.0);
  DUO_CHECK(stats.percentile(0.99) == 0.0);
  DUO_CHECK(stats.count() == 0);
}
