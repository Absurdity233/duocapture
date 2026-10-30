#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace duo {

// 单生产者 / 单消费者无锁环形队列。
//
// 约束：容量必须是 2 的幂；实际可用容量为 Capacity - 1。
// 管线里所有跨线程队列都用它——任何一处加锁在 120 fps 下都会变成抖动源。
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "capacity must be >= 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

 public:
  static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

  bool try_push(const T& value) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & kMask;
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    slots_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_push(T&& value) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & kMask;
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    slots_[head] = std::move(value);
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    out = std::move(slots_[tail]);
    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
  }

  std::size_t size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & kMask;
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  std::array<T, Capacity> slots_{};
  alignas(64) std::atomic<std::size_t> head_{0};  // 只由生产者写
  alignas(64) std::atomic<std::size_t> tail_{0};  // 只由消费者写
};

}  // namespace duo
