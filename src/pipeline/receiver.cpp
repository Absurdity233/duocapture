#include "duocapture/pipeline/receiver.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/types.h"
#include "duocapture/common/log.h"

namespace duo {
namespace {

constexpr u32 kRecvBufferSize = 16u * 1024u * 1024u;
constexpr u32 kMaxDatagram = 2048;

}  // namespace

Receiver::Receiver() : jitter_(8, 3) {}

Receiver::~Receiver() { stop(); }

Status Receiver::start(const ReceiverConfig& config) {
  if (running_.load(std::memory_order_acquire)) {
    return Status::InvalidArgument;
  }

  config_ = config;
  // set_analysis_plugin 是「强制开启」语义，不能被传入的 config 覆盖掉
  if (analysis_forced_) {
    config_.enable_analysis = true;
  }
  stop_requested_.store(false, std::memory_order_release);
  window_closed_.store(false, std::memory_order_release);

  Status status = socket_.open("0.0.0.0", config_.listen_port);
  if (!ok(status)) {
    DUO_ERROR("绑定 %u 失败: %s", static_cast<unsigned>(config_.listen_port), to_string(status));
    return status;
  }
  socket_.set_recv_buffer(kRecvBufferSize);

  jitter_ = JitterBuffer(config_.jitter_window, config_.nack_delay_ms);

  // 控制通道：绑定任意端口即可（发送端不需要回连我们）
  if (!config_.peer_host.empty()) {
    const Status cs = control_socket_.open("0.0.0.0", 0);
    if (!ok(cs)) {
      DUO_WARN("控制通道打开失败: %s（NACK 与关键帧请求不可用）", to_string(cs));
    }
  }

  decoder_ = create_decoder();
  if (!decoder_) {
    DUO_ERROR("没有可用的解码后端");
    socket_.close();
    return Status::NotSupported;
  }

  if (config_.show_window) {
    presenter_ = std::make_unique<Win32Presenter>();

    // 先建窗口再建解码器：解码器需要与呈现器共用同一块 D3D11 设备，
    // 这样解码输出的纹理才能直接送进视频处理器，不需要拷贝。
    const u32 initial_width = config_.width != 0 ? config_.width : 1280;
    const u32 initial_height = config_.height != 0 ? config_.height : 720;
    status = presenter_->initialize(L"DuoCapture", initial_width, initial_height);
    if (!ok(status)) {
      DUO_ERROR("创建呈现窗口失败: %s", to_string(status));
      socket_.close();
      return status;
    }

    if (config_.show_overlay) {
      const Status os = overlay_.initialize(presenter_->device(), presenter_->context());
      if (ok(os)) {
        presenter_->set_overlay(&overlay_);
      } else {
        DUO_WARN("遥测 overlay 初始化失败: %s", to_string(os));
      }
    }

    // 把 D3D 设备交给插件（插件可能要跑 GPU 推理或读回纹理）
    if (device_ready_fn_ != nullptr) {
      device_ready_fn_(device_ready_user_, presenter_->device(), presenter_->context());
    }

    status = decoder_->bind_device(presenter_->device(), presenter_->context());
    if (!ok(status)) {
      DUO_ERROR("解码器无法绑定呈现器的 D3D11 设备: %s", to_string(status));
      socket_.close();
      return status;
    }
  }

  if (config_.enable_analysis && analysis_callback_ != nullptr) {
    const Status as = analysis_.start(analysis_callback_, analysis_user_);
    if (!ok(as)) {
      DUO_WARN("分析 tap 启动失败: %s", to_string(as));
    }
  }

  running_.store(true, std::memory_order_release);
  receive_thread_ = std::thread([this] { receive_loop(); });
  if (presenter_) {
    render_thread_ = std::thread([this] { render_loop(); });
  }

  DUO_INFO("接收端已启动: 监听 %u", static_cast<unsigned>(socket_.local_port()));
  return Status::Ok;
}

void Receiver::receive_loop() {
  std::vector<u8> buffer(kMaxDatagram);

  while (!stop_requested_.load(std::memory_order_acquire)) {
    std::size_t received = 0;
    std::string from_host;
    u16 from_port = 0;

    const Status status =
        socket_.recv_from(duo::MutByteSpan(buffer.data(), buffer.size()), received, from_host,
                          from_port, 100);
    if (status == Status::Timeout) {
      continue;
    }
    if (!ok(status)) {
      DUO_ERROR("接收失败: %s", to_string(status));
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.datagrams_received;
    }

    PacketHeader header{};
    ByteSpan payload;
    if (!parse(ByteSpan(buffer.data(), received), header, payload)) {
      continue;
    }

    // 编码格式随包头发送，首帧到达时据此选解码器。
    if (!decoder_configured_) {
      const u8 codec_field = static_cast<u8>(header.reserved >> 8);
      codec_ = codec_field == 0 ? Codec::H264 : (codec_field == 2 ? Codec::Av1 : Codec::Hevc);
    }

    Packet packet;
    packet.header = header;
    packet.payload.assign(payload.begin(), payload.end());

    // 音频和视频走不同的缓冲：音频不进抖动缓冲（它没有帧间依赖，
    // 而且一旦重排就会造成可听见的断音），直接在收到时拼装播放。
    if (packet.is_audio()) {
      handle_audio_packet(packet);
      continue;
    }

    jitter_.insert(std::move(packet));

    AssembledFrame frame;
    while (jitter_.try_pop(frame)) {
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.frames_assembled;
      }
      if (!presenter_) {
        continue;  // 无窗口模式只统计
      }
      if (decoder_ == nullptr) {
        continue;
      }

      // 必须从关键帧开始。解码器配置也要用关键帧来做——SPS/PPS 只出现在
      // 关键帧里，拿普通帧去提取只会得到空的 sequence header，解码器
      // 随后会从别处猜出错误的分辨率（实测会报 1920x1080 而不是实际的
      // 2560x1600），进而 ProcessOutput 直接 E_FAIL。
      if (!seen_keyframe_) {
        ++frames_since_start_;
        if (!frame.key) {
          // 用 intra refresh 时只有第一帧是 IDR，错过就再也等不到。
          // 等 120 帧还没遇到关键帧就强行开始。
          if (frames_since_start_ < 120) {
            continue;
          }
          DUO_WARN("等待关键帧超时，从第 %u 帧强行开始解码", frame.frame_id);
        } else {
          DUO_INFO("从关键帧 %u 开始解码", frame.frame_id);
        }
        seen_keyframe_ = true;
      }

      if (!decoder_configured_) {
        const Status decode_status = ensure_decoder(frame, config_.width, config_.height);
        if (!ok(decode_status)) {
          continue;
        }
      }

      DecodedFrame decoded;
      const Status decode_status =
          decoder_->decode(ByteSpan(frame.data.data(), frame.data.size()), frame.capture_ts_ns,
                           decoded);
      if (decode_status == Status::WouldBlock) {
        continue;
      }
      if (!ok(decode_status)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.decoder_errors;
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        // 旧帧如果还没被呈现线程取走，直接释放——只保留最新帧。
        if (has_frame_) {
          decoder_->release(latest_);
          std::lock_guard<std::mutex> stats_lock(stats_mutex_);
          ++stats_.frames_dropped;
        }
        latest_ = decoded;
        has_frame_ = true;
        latest_arrival_ns_ = Clock::now_ns();

        // 分析 tap 与呈现完全独立：它只拿纹理指针，忙则丢帧，
        // 不会拖慢解码线程。
        if (config_.enable_analysis) {
          analysis_.submit(decoded);
        }
      }
      frame_cv_.notify_one();
    }
  }

  send_nacks();
}

void Receiver::send_nacks() {
  if (!control_socket_.is_open() || config_.peer_host.empty()) {
    return;
  }

  const u64 now = Clock::now_ns();
  // 每 5ms 最多发一次，避免 NACK 风暴
  if (last_nack_ns_ != 0 && now - last_nack_ns_ < 5'000'000ull) {
    return;
  }

  const auto missing = jitter_.collect_nacks(now);
  if (!missing.empty()) {
    const auto ranges = compact_nacks(missing);
    if (!ranges.empty()) {
      const Bytes msg = serialize_control(
          ControlType::Nack,
          ByteSpan(reinterpret_cast<const u8*>(ranges.data()),
                   ranges.size() * sizeof(NackRange)));
      if (ok(control_socket_.send_to(ByteSpan(msg.data(), msg.size()), config_.peer_host,
                                     config_.peer_control_port))) {
        last_nack_ns_ = now;
      }
    }
  }

  // 长时间等不到关键帧就主动要一个（每 500ms 最多一次）
  if (!seen_keyframe_ && (last_idr_request_ns_ == 0 ||
                          now - last_idr_request_ns_ > 500'000'000ull)) {
    const Bytes msg = serialize_control(ControlType::RequestIdr, {});
    if (ok(control_socket_.send_to(ByteSpan(msg.data(), msg.size()), config_.peer_host,
                                   config_.peer_control_port))) {
      last_idr_request_ns_ = now;
      DUO_INFO("请求关键帧");
    }
  }
}

void Receiver::handle_audio_packet(const Packet& packet) {
  if (!config_.play_audio) {
    return;
  }

  Bytes assembled;
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);

    auto& bucket = audio_pending_[packet.header.frame_id];
    bucket.push_back(packet);

    if (bucket.size() < packet.header.pkt_count) {
      return;  // 还没收齐
    }

    // 按 pkt_index 排序后拼接
    std::sort(bucket.begin(), bucket.end(), [](const Packet& a, const Packet& b) {
      return a.header.pkt_index < b.header.pkt_index;
    });
    for (const Packet& p : bucket) {
      assembled.insert(assembled.end(), p.payload.begin(), p.payload.end());
    }
    audio_pending_.erase(packet.header.frame_id);

    // 攒太多说明有空洞，清掉最老的，避免内存涨
    while (audio_pending_.size() > 64) {
      audio_pending_.erase(audio_pending_.begin());
    }
  }

  if (!audio_started_) {
    if (!ok(audio_decoder_.open())) {
      DUO_WARN("AAC 解码器启动失败，关闭音频播放");
      config_.play_audio = false;
      return;
    }
    if (!ok(audio_render_.start(audio_decoder_.sample_rate(), audio_decoder_.channels()))) {
      DUO_WARN("音频播放启动失败，关闭音频播放");
      config_.play_audio = false;
      audio_decoder_.close();
      return;
    }
    audio_started_ = true;
  }

  std::vector<i16> pcm;
  const Status st = audio_decoder_.decode(ByteSpan(assembled.data(), assembled.size()), pcm);
  if (!ok(st)) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.audio_decode_errors;
    return;
  }
  if (!pcm.empty()) {
    audio_render_.write(ByteSpan(reinterpret_cast<const u8*>(pcm.data()),
                                 pcm.size() * sizeof(i16)));
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.audio_frames;
  }
}

Status Receiver::ensure_decoder(const AssembledFrame& first_frame, u32 width, u32 height) {
  if (decoder_configured_ && width_ == width && height_ == height) {
    return Status::Ok;
  }

  Status status = decoder_->configure(
      codec_, ByteSpan(first_frame.data.data(), first_frame.data.size()));
  if (!ok(status)) {
    return status;
  }

  width_ = width;
  height_ = height;
  decoder_configured_ = true;
  DUO_INFO("解码器已配置: %ux%u", width, height);

  if (presenter_ && config_.width != 0 && config_.height != 0) {
    // 尺寸由调用方指定时保持窗口尺寸，否则让解码器自适应
  }
  return Status::Ok;
}

void Receiver::render_loop() {
  u64 window_frames = 0;
  u64 last_stats_ns = Clock::now_ns();

  while (!stop_requested_.load(std::memory_order_acquire)) {
    DecodedFrame frame{};
    u64 arrival = 0;
    {
      std::unique_lock<std::mutex> lock(frame_mutex_);
      frame_cv_.wait_for(lock, std::chrono::milliseconds(50),
                         [this] { return has_frame_ || stop_requested_.load(); });
      if (!has_frame_) {
        continue;
      }
      frame = latest_;
      arrival = latest_arrival_ns_;
      has_frame_ = false;
    }

    const Status status = presenter_->present(frame);
    decoder_->release(frame);

    if (!ok(status) && status != Status::Closed) {
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.frames_presented;
      if (arrival != 0) {
        const double e2e = Clock::to_ms(Clock::now_ns() - arrival);
        stats_.end_to_end_ms = e2e;
        latency_stats_.add(e2e);
      }
    }
    ++window_frames;

    const u64 now = Clock::now_ns();
    if (now - last_stats_ns >= 1'000'000'000ull) {
      const double seconds = Clock::to_ms(now - last_stats_ns) / 1000.0;
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.receive_fps = seconds > 0.0 ? static_cast<double>(window_frames) / seconds : 0.0;
      }
      last_stats_ns = now;
      window_frames = 0;

      if (config_.show_overlay) {
        const ReceiverStats snapshot = stats();
        char buf[64];
        std::vector<OverlayLine> lines;

        std::snprintf(buf, sizeof(buf), "%.1f fps", snapshot.receive_fps);
        lines.push_back({"fps", buf});
        std::snprintf(buf, sizeof(buf), "%.1f ms", snapshot.p50_end_to_end_ms);
        lines.push_back({"latency p50", buf});
        std::snprintf(buf, sizeof(buf), "%.1f ms", snapshot.p99_end_to_end_ms);
        lines.push_back({"latency p99", buf});
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(snapshot.frames_presented));
        lines.push_back({"frames", buf});
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(snapshot.frames_dropped));
        lines.push_back({"dropped", buf});
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(snapshot.packets_recovered_by_fec));
        lines.push_back({"fec recovered", buf});
        std::snprintf(buf, sizeof(buf), "%.1f ms", snapshot.arrival_jitter_ms);
        lines.push_back({"jitter", buf});
        std::snprintf(buf, sizeof(buf), "%u ms", snapshot.nack_delay_ms);
        lines.push_back({"nack delay", buf});
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(snapshot.decoder_errors));
        lines.push_back({"decode errors", buf});
        if (config_.enable_analysis) {
          std::snprintf(buf, sizeof(buf), "%llu (%llu)",
                        static_cast<unsigned long long>(snapshot.frames_analyzed),
                        static_cast<unsigned long long>(snapshot.frames_analysis_dropped));
          lines.push_back({"analyzed", buf});
        }
        if (config_.play_audio) {
          std::snprintf(buf, sizeof(buf), "%llu",
                        static_cast<unsigned long long>(snapshot.audio_frames));
          lines.push_back({"audio frames", buf});
        }
        overlay_.set_lines(lines);
      }
    }
  }
}

bool Receiver::pump() {
  if (presenter_ == nullptr) {
    return !window_closed_.load(std::memory_order_acquire);
  }
  const bool alive = presenter_->pump(false);
  if (presenter_->should_close()) {
    window_closed_.store(true, std::memory_order_release);
    return false;
  }
  return alive;
}

void Receiver::stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  stop_requested_.store(true, std::memory_order_release);
  frame_cv_.notify_all();

  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
  if (render_thread_.joinable()) {
    render_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (has_frame_ && decoder_) {
      decoder_->release(latest_);
      has_frame_ = false;
    }
  }

  analysis_.stop();
  audio_render_.stop();
  audio_decoder_.close();
  control_socket_.close();

  if (decoder_) {
    decoder_.reset();  // 必须在呈现器之前销毁：它引用了呈现器的 D3D 设备
  }
  presenter_.reset();
  socket_.close();
  running_.store(false, std::memory_order_release);

  DUO_INFO("接收端已停止");
}

ReceiverStats Receiver::stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ReceiverStats out = stats_;
  out.packets_recovered_by_fec = jitter_.stats().packets_recovered;
  out.frames_dropped = std::max(out.frames_dropped, jitter_.stats().frames_dropped);
  out.frames_analyzed = analysis_.frames_analyzed();
  out.frames_analysis_dropped = analysis_.frames_dropped();
  out.arrival_jitter_ms = jitter_.stats().arrival_jitter_ms;
  out.nack_delay_ms = jitter_.stats().effective_nack_delay_ms;
  out.nacks_issued = jitter_.stats().nacks_issued;
  out.audio_frames = stats_.audio_frames;
  out.audio_decode_errors = stats_.audio_decode_errors;
  out.p50_end_to_end_ms = latency_stats_.percentile(0.5);
  out.p99_end_to_end_ms = latency_stats_.percentile(0.99);
  return out;
}

Win32Presenter* Receiver::presenter() noexcept { return presenter_.get(); }

void Receiver::set_analysis_callback(AnalysisTap::Callback callback, void* user) {
  analysis_callback_ = callback;
  analysis_user_ = user;
}

void Receiver::set_device_ready_callback(void (*fn)(void* user, void* device, void* context),
                                         void* user) {
  device_ready_fn_ = fn;
  device_ready_user_ = user;
}

void Receiver::set_analysis_plugin(void* adapter, AnalysisTap::Callback thunk, void* user) {
  (void)adapter;
  analysis_callback_ = thunk;
  analysis_user_ = user;
  config_.enable_analysis = true;
  analysis_forced_ = true;
}

}  // namespace duo
