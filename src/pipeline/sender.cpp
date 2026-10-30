#include "duocapture/pipeline/sender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "duocapture/encode/nvenc_encoder.h"
#include "duocapture/net/fec.h"
#include "duocapture/net/packetizer.h"

namespace duo {
namespace {

// 单次 send 循环里最多取这么多包，避免长时间占用限速器的锁。
constexpr std::size_t kMaxPacketsPerPoll = 256;

}  // namespace

Sender::Sender() : pacer_(1) {}

Sender::~Sender() { stop(); }

Status Sender::start(const SenderConfig& config) {
  if (running_.load(std::memory_order_acquire)) {
    return Status::InvalidArgument;
  }

  config_ = config;
  stop_requested_.store(false, std::memory_order_release);

  Status status = setup_pipeline();
  if (!ok(status)) {
    return status;
  }

  running_.store(true, std::memory_order_release);
  capture_thread_ = std::thread([this] { capture_loop(); });
  send_thread_ = std::thread([this] { send_loop(); });

  DUO_INFO("发送端已启动: %ux%u @%u, %u Mbps, -> %s:%u",
           width_, height_, config_.fps, config_.bitrate_bps / 1'000'000,
           config_.target_host.c_str(), static_cast<unsigned>(config_.target_port));
  return Status::Ok;
}

Status Sender::setup_pipeline() {
  // 回放模式：不采集也不编码，直接把裸流按帧切开发出去。
  // 用来把「传输 → 解码 → 呈现」这段从采集/live 画面里隔离出来验证。
  if (!config_.playback_path.empty()) {
    Status status = socket_.open("0.0.0.0", 0);
    if (!ok(status)) {
      return status;
    }
    pacer_ = Pacer(static_cast<u64>(config_.bitrate_bps) / 8 * 2);
    return Status::Ok;
  }

  capture_ = create_capture_source(config_.prefer_wgc);
  if (!capture_) {
    DUO_ERROR("没有可用的采集后端");
    return Status::NotSupported;
  }

  Status status = capture_->start(config_.display_index);
  if (!ok(status)) {
    DUO_ERROR("启动采集失败: %s", to_string(status));
    return Status::NotSupported;
  }

  device_ = capture_->d3d_device();
  context_ = capture_->d3d_context();
  if (device_ == nullptr) {
    DUO_ERROR("采集后端没有提供 D3D11 设备");
    return Status::NotSupported;
  }

  auto* device = static_cast<ID3D11Device*>(device_);
  auto* context = static_cast<ID3D11DeviceContext*>(context_);

  status = converter_.initialize(device, context);
  if (!ok(status)) {
    DUO_ERROR("色彩转换初始化失败: %s", to_string(status));
    return Status::NotSupported;
  }

  auto nvenc = std::make_unique<NvencEncoder>();
  status = nvenc->initialize(device, context);
  if (!ok(status)) {
    DUO_ERROR("NVENC 初始化失败: %s", to_string(status));
    return Status::NotSupported;
  }

  // 先用采集报告的尺寸配置编码器；尺寸变化时在循环里重建。
  // 这里取一帧来拿到真实尺寸。
  FrameView probe;
  status = capture_->acquire(probe, 2000);
  if (!ok(status)) {
    DUO_ERROR("采集首帧超时，无法确定分辨率: %s", to_string(status));
    return Status::NotSupported;
  }
  width_ = probe.width;
  height_ = probe.height;
  capture_->release(probe);

  EncoderConfig encoder_config{};
  encoder_config.codec = config_.codec;
  encoder_config.width = width_;
  encoder_config.height = height_;
  encoder_config.fps = config_.fps;
  encoder_config.bitrate_bps = config_.bitrate_bps;
  encoder_config.intra_refresh = config_.intra_refresh;
  encoder_config.gop_frames = 0;
  encoder_config.slice_count = 4;

  status = nvenc->configure(encoder_config);
  if (!ok(status)) {
    DUO_ERROR("NVENC 配置失败: %s", to_string(status));
    return Status::NotSupported;
  }
  encoder_ = std::move(nvenc);

  status = socket_.open("0.0.0.0", 0);
  if (!ok(status)) {
    DUO_ERROR("打开发送套接字失败: %s", to_string(status));
    return Status::NotSupported;
  }
  socket_.set_send_buffer(8u * 1024u * 1024u);

  // 限速器的目标速率。
  //
  // 注意这不是"带宽上限"，而是"平滑发送节奏"的目标：局域网带宽充足，
  // 限速器的作用只是把一帧的包摊开到几毫秒里，避免整帧突发打满交换机
  // 端口队列。所以取值要比编码器实际输出宽裕——编码器的 CBR 在场景
  // 切换时会有瞬时的码率上冲，若限速器卡在标称码率上，包就会在
  // pending_ 里积压，最终触发整帧丢弃。
  //
  // 给 2 倍余量：既能吸收 CBR 的上冲，又仍然对真正的突发有约束。
  const u64 bytes_per_second =
      config_.pacer_bytes_per_second != 0
          ? config_.pacer_bytes_per_second
          : static_cast<u64>(config_.bitrate_bps) / 8 * 2;
  pacer_ = Pacer(bytes_per_second);

  // 调试用：把编码后的裸码流落盘，方便离线验证解码器。
  if (!config_.record_path.empty()) {
    record_.open(config_.record_path, std::ios::binary | std::ios::trunc);
    if (record_) {
      DUO_INFO("码流录制到 %s", config_.record_path.c_str());
    } else {
      DUO_WARN("无法打开录制文件 %s", config_.record_path.c_str());
    }
  }

  return Status::Ok;
}

void Sender::capture_loop() {
  // 回放模式：读裸流，按 Annex-B 起始码切帧，每帧按目标帧率发出去。
  // 只用于验证「传输 → 解码 → 呈现」，不参与真实采集。
  if (!config_.playback_path.empty()) {
    std::ifstream file(config_.playback_path, std::ios::binary);
    if (!file) {
      DUO_ERROR("无法打开回放文件 %s", config_.playback_path.c_str());
      stop_requested_.store(true, std::memory_order_release);
      return;
    }
    const Bytes data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DUO_INFO("回放 %s: %zu 字节", config_.playback_path.c_str(), data.size());

    // 找出所有起始码位置
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 4 <= data.size(); ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
        starts.push_back(i);
        i += 3;
      }
    }
    if (starts.empty()) {
      DUO_ERROR("回放文件里没有 Annex-B 起始码");
      stop_requested_.store(true, std::memory_order_release);
      return;
    }

    const auto* codec_bytes = data.data();
    const u8 first_nal = data[starts.front() + 4];
    const bool h264 = (first_nal & 0x1F) == 7 || (first_nal & 0x1F) == 5;

    Packetizer packetizer;
    const auto frame_interval = std::chrono::milliseconds(
        config_.playback_fps == 0 ? 50 : 1000 / config_.playback_fps);
    auto next_deadline = std::chrono::steady_clock::now();
    std::size_t accumulated_begin = 0;
    bool key_pending = true;

    for (std::size_t k = 0; k < starts.size() && !stop_requested_.load(); ++k) {
      const std::size_t begin = starts[k];
      const std::size_t end = (k + 1 < starts.size()) ? starts[k + 1] : data.size();
      if (begin + 5 >= end) {
        continue;
      }
      const u8 header = codec_bytes[begin + 4];
      const bool is_vcl = h264 ? (((header & 0x1F) == 1) || ((header & 0x1F) == 5))
                               : ((((header >> 1) & 0x3F) <= 31));
      if (!is_vcl) {
        continue;
      }

      // 第一帧带上 SPS/PPS（accumulated_begin == 0），后续每帧独立。
      const std::size_t frame_begin = key_pending ? accumulated_begin : begin;
      key_pending = false;

      FrameMeta meta{};
      meta.frame_id = next_frame_id_++;
      meta.capture_ts_ns = Clock::now_ns();
      meta.key = (k == 0) || h264 ? (header & 0x1F) == 5 : (((header >> 1) & 0x3F) == 19);
      meta.codec = h264 ? 0 : 1;
      meta.width = width_;
      meta.height = height_;

      auto packets = packetizer.packetize(
          meta, ByteSpan(codec_bytes + frame_begin, end - frame_begin));
      if (packets.empty()) {
        continue;
      }
      if (config_.enable_fec) {
        auto parity = FecCodec::encode_frame(packets, config_.fec_group_size);
        for (auto& check : parity) {
          packets.push_back(std::move(check));
        }
      }

      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.frames_captured;
        ++stats_.frames_encoded;
      }
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto& packet : packets) {
          pending_.push_back(std::move(packet));
        }
      }
      queue_cv_.notify_one();

      next_deadline += frame_interval;
      std::this_thread::sleep_until(next_deadline);
    }

    DUO_INFO("回放结束");
    stop_requested_.store(true, std::memory_order_release);
    queue_cv_.notify_all();
    return;
  }

  const auto* device = static_cast<ID3D11Device*>(device_);
  Packetizer packetizer;

  u64 last_stats_ns = Clock::now_ns();
  u64 window_frames = 0;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    FrameView view;
    Status status = capture_->acquire(view, 200);

    if (status == Status::Timeout) {
      continue;
    }
    if (status == Status::Closed) {
      DUO_WARN("采集会话失效，重建");
      capture_->stop();
      if (!ok(capture_->start(config_.display_index))) {
        DUO_ERROR("采集重建失败，停止发送");
        stop_requested_.store(true, std::memory_order_release);
        break;
      }
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.capture_restarts;
      }
      continue;
    }
    if (!ok(status)) {
      DUO_ERROR("采集失败: %s", to_string(status));
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.frames_captured;
    }
    ++window_frames;

    // 分辨率变化：重建编码器
    if (view.width != width_ || view.height != height_) {
      DUO_INFO("分辨率变化 %ux%u -> %ux%u，重建编码器", width_, height_, view.width, view.height);
      width_ = view.width;
      height_ = view.height;

      EncoderConfig encoder_config{};
      encoder_config.codec = config_.codec;
      encoder_config.width = width_;
      encoder_config.height = height_;
      encoder_config.fps = config_.fps;
      encoder_config.bitrate_bps = config_.bitrate_bps;
      encoder_config.intra_refresh = config_.intra_refresh;
      encoder_config.slice_count = 4;
      encoder_->configure(encoder_config);
    }

    ID3D11Texture2D* nv12 = nullptr;
    status = converter_.convert(static_cast<ID3D11Texture2D*>(view.texture), &nv12);
    capture_->release(view);
    if (!ok(status)) {
      DUO_ERROR("色彩转换失败: %s", to_string(status));
      continue;
    }

    FrameView nv12_view{};
    nv12_view.texture = nv12;
    nv12_view.width = width_;
    nv12_view.height = height_;
    nv12_view.capture_ts_ns = view.capture_ts_ns;

    EncodedFrame encoded;
    status = encoder_->encode(nv12_view, encoded);
    if (!ok(status)) {
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.encode_errors;
      }
      continue;
    }

    if (record_.is_open()) {
      record_.write(reinterpret_cast<const char*>(encoded.data.data()),
                    static_cast<std::streamsize>(encoded.data.size()));
    }

    FrameMeta meta{};
    meta.frame_id = next_frame_id_++;
    meta.capture_ts_ns = encoded.capture_ts_ns;
    meta.key = encoded.key;
    meta.codec = static_cast<u8>(config_.codec);
    meta.width = width_;
    meta.height = height_;

    auto packets = packetizer.packetize(meta, encoded.data);
    if (packets.empty()) {
      continue;
    }
    if (config_.enable_fec) {
      auto parity = FecCodec::encode_frame(packets, config_.fec_group_size);
      for (auto& check : parity) {
        packets.push_back(std::move(check));
      }
    }

    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.frames_encoded;
      stats_.avg_encode_ms = stats_.avg_encode_ms * 0.9 +
                             static_cast<double>(stats_.frames_encoded) * 0.0;  // 由 nvenc 统计
    }

    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      // 积压超过两帧的包量说明发送端跟不上，按整帧丢弃最旧的数据。
      //
      // 注意必须按帧边界丢：如果从中间截断，接收端永远拼不出完整帧，
      // 表现为"包都在收、组装数恒为 0"。这里从队首丢掉若干完整帧，
      // 一直丢到剩余容量够放新帧为止。
      const std::size_t max_pending = 4096;
      if (pending_.size() + packets.size() > max_pending) {
        // 找到可以安全丢弃的帧边界：下一个 kFlagFrameStart。
        std::size_t drop_to = 0;
        while (drop_to < pending_.size() &&
               pending_.size() - drop_to + packets.size() > max_pending) {
          std::size_t next = drop_to + 1;
          while (next < pending_.size() &&
                 (pending_[next].header.flags & kFlagFrameStart) == 0) {
            ++next;
          }
          drop_to = next;
        }
        if (drop_to > 0) {
          DUO_WARN("发送队列积压，按整帧丢弃 %zu 个包以控制延迟", drop_to);
          pending_.erase(pending_.begin(),
                         pending_.begin() + static_cast<std::ptrdiff_t>(drop_to));
        }
      }
      for (auto& packet : packets) {
        pending_.push_back(std::move(packet));
      }
    }
    queue_cv_.notify_one();

    const u64 now = Clock::now_ns();
    if (now - last_stats_ns >= 1'000'000'000ull) {
      const double seconds = Clock::to_ms(now - last_stats_ns) / 1000.0;
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.fps = seconds > 0.0 ? static_cast<double>(window_frames) / seconds : 0.0;
      last_stats_ns = now;
      window_frames = 0;
    }
  }

  (void)device;
}

void Sender::send_loop() {
  std::vector<Packet> batch;
  std::vector<u8> wire;
  wire.reserve(kMaxPacketSize);

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // 先把现有积压搬进限速器。限速器才是真正的队列，pending_ 只做交接，
    // 这样限速器的令牌可以持续被消费，不会被 5 ms 的等待拖住。
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (!pending_.empty()) {
        batch.clear();
        batch.swap(pending_);
      }
    }
    for (auto& packet : batch) {
      pacer_.enqueue(std::move(packet));
    }
    batch.clear();

    const u64 now = Clock::now_ns();
    Packet ready[kMaxPacketsPerPoll];
    const std::size_t count = pacer_.poll(now, ready, kMaxPacketsPerPoll);
    for (std::size_t i = 0; i < count; ++i) {
      wire = serialize(ready[i].header, ready[i].payload);
      const Status status =
          socket_.send_to(ByteSpan(wire.data(), wire.size()), config_.target_host,
                          config_.target_port);
      if (ok(status)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.packets_sent;
        stats_.bytes_sent += wire.size();
        // 只统计完整发出去的帧：帧尾包成功发送时记一次。
        if ((ready[i].header.flags & kFlagFrameEnd) != 0 && !ready[i].is_parity()) {
          ++stats_.frames_sent;
        }
      }
    }

    // 节奏控制：
    //   * pacer 还有积压但本次没放行（令牌不足）—— 只需要等令牌补足，
    //     用 0.5 ms 粒度睡，避免发送线程长时间不在岗导致上游积压
    //   * pacer 空了 —— 等生产者，用条件变量唤醒而不是固定超时
    if (!pacer_.empty()) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    } else {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(2),
                         [this] { return !pending_.empty() || stop_requested_.load(); });
    }
  }
}

void Sender::stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  stop_requested_.store(true, std::memory_order_release);
  queue_cv_.notify_all();

  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  if (send_thread_.joinable()) {
    send_thread_.join();
  }

  if (capture_) {
    capture_->stop();
    capture_.reset();
  }
  converter_.shutdown();
  encoder_.reset();
  if (record_.is_open()) {
    record_.close();
  }
  socket_.close();
  running_.store(false, std::memory_order_release);

  DUO_INFO("发送端已停止");
}

SenderStats Sender::stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  SenderStats out = stats_;
  if (auto* nvenc = dynamic_cast<NvencEncoder*>(encoder_.get())) {
    out.avg_encode_ms = nvenc->stats().avg_encode_ms;
  }
  return out;
}

}  // namespace duo
