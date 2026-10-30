#include "duocapture/net/pacer.h"

#include <algorithm>

namespace duo {

Pacer::Pacer(u64 bytes_per_second, u64 burst_bytes)
    : bytes_per_second_(bytes_per_second == 0 ? 1 : bytes_per_second) {
  // burst 取 100 ms 的流量。这是延迟与吞吐的折中：
  //   * 太小（1 ms）时，一帧的包要跨多次 poll 才发完，而每次 poll 之间有
  //     等待，有效发送速率会被压到远低于目标码率，队列越积越多
  //   * 太大则整帧突发，打满交换机端口队列，产生毫秒级延迟尖峰
  // 100 ms 仍然限制了单次突发上限，同时足够让一帧在 1-2 次 poll 内发完。
  burst_ = burst_bytes != 0 ? static_cast<double>(burst_bytes)
                            : static_cast<double>(bytes_per_second_) / 10.0;
  burst_ = std::max(burst_, static_cast<double>(kMaxPacketSize * 2));
  tokens_ = burst_;
}

void Pacer::set_bitrate(u64 bytes_per_second) noexcept {
  bytes_per_second_ = bytes_per_second == 0 ? 1 : bytes_per_second;
}

void Pacer::enqueue(Packet packet) { queue_.push_back(std::move(packet)); }

void Pacer::refill(u64 now_ns) noexcept {
  if (last_ns_ == 0) {
    last_ns_ = now_ns;
    return;
  }
  if (now_ns <= last_ns_) {
    return;
  }
  const double elapsed_s = static_cast<double>(now_ns - last_ns_) / 1e9;
  last_ns_ = now_ns;
  tokens_ = std::min(burst_, tokens_ + elapsed_s * static_cast<double>(bytes_per_second_));
}

std::size_t Pacer::poll(u64 now_ns, Packet* out, std::size_t max_out) {
  refill(now_ns);

  std::size_t produced = 0;
  while (produced < max_out && !queue_.empty()) {
    const double cost = static_cast<double>(queue_.front().total_size());
    if (tokens_ < cost) {
      break;
    }
    tokens_ -= cost;
    out[produced++] = std::move(queue_.front());
    queue_.pop_front();
  }
  return produced;
}

}  // namespace duo
