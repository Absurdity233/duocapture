#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/decode/decoder.h"

namespace duo {

// 分析 tap：把解码后的帧交给下游消费者（CV / OCR / 质检）。
//
// 与呈现线程的关键区别是**丢帧语义相反**：
//   * 呈现：只显示最新帧，旧的直接丢 —— 延迟优先
//   * 分析：忙就跳过这一帧 —— 吞吐优先，绝不阻塞解码
//
// 所以两者必须是独立的队列，不能共用一个。这里用「最新帧 + 忙标志」：
// 分析线程慢的时候，新帧直接覆盖未消费的旧帧，解码线程永远不等待。
class AnalysisTap {
 public:
  using Callback = void (*)(void* user, const DecodedFrame& frame);

  AnalysisTap();
  ~AnalysisTap();

  AnalysisTap(const AnalysisTap&) = delete;
  AnalysisTap& operator=(const AnalysisTap&) = delete;

  // 启动分析线程。callback 在分析线程里调用。
  Status start(Callback callback, void* user);

  // 由解码线程调用：提交一帧。**永不阻塞**。如果分析线程还没消费
  // 上一帧，这一帧会覆盖它并计入 dropped。
  void submit(const DecodedFrame& frame);

  void stop();

  [[nodiscard]] u64 frames_submitted() const noexcept;
  [[nodiscard]] u64 frames_analyzed() const noexcept;
  [[nodiscard]] u64 frames_dropped() const noexcept;

 private:
  void loop();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 遥测快照：给 UI / 日志用。
struct Telemetry {
  // 网络
  u64 packets_received = 0;
  u64 packets_lost = 0;
  u64 fec_recovered = 0;
  double loss_rate = 0.0;

  // 帧
  u64 frames_assembled = 0;
  u64 frames_presented = 0;
  u64 frames_dropped = 0;
  u64 frames_analyzed = 0;
  u64 frames_analysis_dropped = 0;

  // 延迟（毫秒）
  double decode_ms = 0.0;
  double present_ms = 0.0;
  double end_to_end_ms = 0.0;
  double p99_end_to_end_ms = 0.0;

  // 吞吐
  double receive_fps = 0.0;
  double bitrate_mbps = 0.0;
};

// 滚动窗口的百分位统计。用来算 p99 延迟这类指标。
class RollingStats {
 public:
  explicit RollingStats(std::size_t window = 600);

  void add(double value);
  [[nodiscard]] double mean() const;
  [[nodiscard]] double percentile(double p) const;  // 0.0 - 1.0
  [[nodiscard]] std::size_t count() const;
  void reset();

 private:
  std::size_t window_;
  std::vector<double> values_;
  std::size_t next_ = 0;
  bool full_ = false;
};

}  // namespace duo
