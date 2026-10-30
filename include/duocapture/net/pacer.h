#pragma once

#include <cstddef>
#include <deque>

#include "duocapture/common/types.h"
#include "duocapture/net/packet.h"

namespace duo {

// 令牌桶限速器。
//
// 存在的唯一理由：一帧编码数据在几百微秒内全吐出去，会让交换机端口队列
// 瞬间打满，随后就是几十毫秒的延迟尖峰。按 1 ms 粒度摊平发送是低延迟
// 链路里收益最大的一件事，比换编码器参数有效得多。
class Pacer {
 public:
  // bytes_per_second：目标发送速率。burst_bytes 为 0 时取 1 ms 的流量。
  explicit Pacer(u64 bytes_per_second, u64 burst_bytes = 0);

  void set_bitrate(u64 bytes_per_second) noexcept;
  [[nodiscard]] u64 bitrate() const noexcept { return bytes_per_second_; }

  void enqueue(Packet packet);

  // 取出当前时刻允许发送的包，最多 max_out 个。返回实际取出数量。
  std::size_t poll(u64 now_ns, Packet* out, std::size_t max_out);

  [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
  [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
  void clear() noexcept { queue_.clear(); }

 private:
  void refill(u64 now_ns) noexcept;

  u64 bytes_per_second_;
  double tokens_;
  double burst_;
  u64 last_ns_ = 0;
  std::deque<Packet> queue_;
};

}  // namespace duo
