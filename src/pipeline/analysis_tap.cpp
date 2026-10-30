#include "duocapture/pipeline/analysis_tap.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "duocapture/common/log.h"

namespace duo {

struct AnalysisTap::Impl {
  Callback callback = nullptr;
  void* user = nullptr;

  std::thread worker;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};

  std::mutex mutex;
  std::condition_variable cv;

  // 只保留最新一帧。分析慢的时候新帧覆盖旧帧，解码线程不等待。
  DecodedFrame latest{};
  bool has_frame = false;
  bool busy = false;

  std::atomic<u64> submitted{0};
  std::atomic<u64> analyzed{0};
  std::atomic<u64> dropped{0};
};

AnalysisTap::AnalysisTap() : impl_(std::make_unique<Impl>()) {}

AnalysisTap::~AnalysisTap() { stop(); }

Status AnalysisTap::start(Callback callback, void* user) {
  if (impl_->running.load(std::memory_order_acquire)) {
    return Status::InvalidArgument;
  }
  if (callback == nullptr) {
    return Status::InvalidArgument;
  }

  impl_->callback = callback;
  impl_->user = user;
  impl_->stop_requested.store(false, std::memory_order_release);
  impl_->running.store(true, std::memory_order_release);
  impl_->worker = std::thread([this] { loop(); });
  return Status::Ok;
}

void AnalysisTap::submit(const DecodedFrame& frame) {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }
  if (frame.texture == nullptr) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    // 上一帧还没被取走就丢掉——分析是「尽量做」，不是「必须做」。
    if (impl_->has_frame) {
      impl_->dropped.fetch_add(1, std::memory_order_relaxed);
    }
    impl_->latest = frame;
    impl_->has_frame = true;
  }
  impl_->submitted.fetch_add(1, std::memory_order_relaxed);
  impl_->cv.notify_one();
}

void AnalysisTap::loop() {
  while (!impl_->stop_requested.load(std::memory_order_acquire)) {
    DecodedFrame frame{};
    {
      std::unique_lock<std::mutex> lock(impl_->mutex);
      impl_->cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
        return impl_->has_frame || impl_->stop_requested.load(std::memory_order_acquire);
      });
      if (!impl_->has_frame) {
        continue;
      }
      frame = impl_->latest;
      impl_->has_frame = false;
    }

    // 回调在锁外执行：分析可能耗时几十毫秒，持锁会阻塞解码线程的 submit。
    if (impl_->callback != nullptr) {
      impl_->callback(impl_->user, frame);
    }
    impl_->analyzed.fetch_add(1, std::memory_order_relaxed);
  }
}

void AnalysisTap::stop() {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }
  impl_->stop_requested.store(true, std::memory_order_release);
  impl_->cv.notify_all();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->running.store(false, std::memory_order_release);

  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->has_frame = false;
}

u64 AnalysisTap::frames_submitted() const noexcept {
  return impl_->submitted.load(std::memory_order_relaxed);
}

u64 AnalysisTap::frames_analyzed() const noexcept {
  return impl_->analyzed.load(std::memory_order_relaxed);
}

u64 AnalysisTap::frames_dropped() const noexcept {
  return impl_->dropped.load(std::memory_order_relaxed);
}

// ---- RollingStats ----

RollingStats::RollingStats(std::size_t window) : window_(window == 0 ? 1 : window) {
  values_.reserve(window_);
}

void RollingStats::add(double value) {
  if (values_.size() < window_) {
    values_.push_back(value);
  } else {
    values_[next_] = value;
    next_ = (next_ + 1) % window_;
    full_ = true;
  }
}

double RollingStats::mean() const {
  if (values_.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (const double v : values_) {
    sum += v;
  }
  return sum / static_cast<double>(values_.size());
}

double RollingStats::percentile(double p) const {
  if (values_.empty()) {
    return 0.0;
  }
  std::vector<double> sorted = values_;
  std::sort(sorted.begin(), sorted.end());
  const double clamped = std::clamp(p, 0.0, 1.0);
  std::size_t index = static_cast<std::size_t>(clamped * static_cast<double>(sorted.size() - 1));
  index = std::min(index, sorted.size() - 1);
  return sorted[index];
}

std::size_t RollingStats::count() const { return values_.size(); }

void RollingStats::reset() {
  values_.clear();
  next_ = 0;
  full_ = false;
}

}  // namespace duo
