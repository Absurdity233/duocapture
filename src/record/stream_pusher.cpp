#include "duocapture/record/stream_pusher.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/common/log.h"
#include "duocapture/record/mkv_muxer.h"

namespace duo {
namespace {

// 管道名里不能有反斜杠，用 PID + 计数保证唯一。
std::string make_pipe_name() {
  static std::atomic<unsigned> counter{0};
  char buf[128];
  std::snprintf(buf, sizeof(buf), "\\\\.\\pipe\\duocapture_push_%lu_%u",
                static_cast<unsigned long>(::GetCurrentProcessId()),
                counter.fetch_add(1, std::memory_order_relaxed));
  return buf;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

struct StreamPusher::Impl {
  // MkvMuxer 的 sink：把字节追加到 pending，由写线程消费
  static void on_mux_bytes(void* user, const u8* data, std::size_t size) {
    auto* self = static_cast<Impl*>(user);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pending.insert(self->pending.end(), data, data + size);
  }

  HANDLE pipe = INVALID_HANDLE_VALUE;
  PROCESS_INFORMATION process{};
  MkvMuxer muxer;
  bool audio_enabled = false;
  u32 audio_rate = 48000;
  u16 audio_channels = 2;
  Bytes audio_private;
  std::thread writer;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};

  std::mutex mutex;
  Bytes pending;          // 待写入管道的字节
  std::atomic<bool> pipe_ready{false};

  std::atomic<u64> frames{0};
  std::atomic<u64> bytes{0};

  Codec codec = Codec::Hevc;
  u32 width = 0;
  u32 height = 0;
  u32 fps = 60;

  // 把 MKV 字节流写进管道。管道写满时会阻塞——这是对的，
  // 说明 ffmpeg 处理不过来，让上游慢下来比丢数据好。
  void writer_loop() {
    while (!stop_requested.load(std::memory_order_acquire)) {
      // 必须先确认管道可用，再取数据。反过来的话，管道没就绪时
      // swap 出来的 chunk 会被直接丢弃——表现为 pending 一直涨、
      // 实际一个字节都没发出去。
      if (!pipe_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }

      Bytes chunk;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending.empty()) {
          chunk.clear();
        } else {
          chunk.swap(pending);
        }
      }

      if (chunk.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }

      DWORD written = 0;
      const BOOL ok_write = ::WriteFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                                        &written, nullptr);
      if (!ok_write) {
        DUO_WARN("推流管道写入失败（ffmpeg 可能已退出）");
        stop_requested.store(true, std::memory_order_release);
        break;
      }
      bytes.fetch_add(written, std::memory_order_relaxed);
    }
  }

  void cleanup() {
    if (pipe != INVALID_HANDLE_VALUE) {
      ::CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
    }
    if (process.hProcess != nullptr) {
      ::CloseHandle(process.hThread);
      ::CloseHandle(process.hProcess);
      process = PROCESS_INFORMATION{};
    }
    pipe_ready.store(false, std::memory_order_release);
  }
};

StreamPusher::StreamPusher() : impl_(std::make_unique<Impl>()) {}

StreamPusher::~StreamPusher() { abort(); }

bool ffmpeg_available(const std::string& path) {
  const std::string exe = path.empty() ? "ffmpeg" : path;
  char resolved[MAX_PATH]{};
  if (::SearchPathA(nullptr, exe.c_str(), ".exe", MAX_PATH, resolved, nullptr) > 0) {
    return true;
  }
  // 允许直接给完整路径
  return ::GetFileAttributesA(exe.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void StreamPusher::set_audio_format(u32 sample_rate, u16 channels,
                                   const Bytes& audio_private) {
  impl_->audio_enabled = true;
  impl_->audio_rate = sample_rate;
  impl_->audio_channels = channels;
  impl_->audio_private = audio_private;
}

Status StreamPusher::push_audio(ByteSpan aac_frame, u64 capture_ts_ns) {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return Status::NotInitialized;
  }
  if (!impl_->audio_enabled) {
    return Status::InvalidArgument;
  }
  return impl_->muxer.write_audio(aac_frame, capture_ts_ns);
}

void StreamPusher::set_video_format(Codec codec, u32 width, u32 height, u32 fps) {
  impl_->codec = codec;
  impl_->width = width;
  impl_->height = height;
  impl_->fps = fps;
}

Status StreamPusher::start(const std::string& url, const std::string& ffmpeg_path) {
  if (impl_->running.load(std::memory_order_acquire)) {
    return Status::InvalidArgument;
  }
  if (url.empty()) {
    return Status::InvalidArgument;
  }
  if (!ffmpeg_available(ffmpeg_path)) {
    DUO_ERROR("找不到 ffmpeg。请安装并加入 PATH，或用 --ffmpeg 指定路径");
    return Status::NotSupported;
  }

  // 用匿名管道接 ffmpeg 的 stdin：`-i -` 让它从标准输入读 MKV。
  //
  // 之前试过命名管道（CreateNamedPipe + ConnectNamedPipe），
  // 但 ffmpeg 打开管道的方式和 PIPE_ACCESS_OUTBOUND 不匹配，
  // WriteFile 会一直阻塞。匿名管道 + 重定向 stdin 简单可靠得多。
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_end = INVALID_HANDLE_VALUE;
  HANDLE write_end = INVALID_HANDLE_VALUE;
  if (!::CreatePipe(&read_end, &write_end, &sa, 1 << 20)) {
    DUO_ERROR("创建管道失败: %lu", ::GetLastError());
    return Status::IoError;
  }
  // 写端不继承，避免 ffmpeg 持有它导致 EOF 收不到
  ::SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);

  // ffmpeg 从 stdin 读 matroska，-c copy 推到目标
  const std::string cmd = quote(ffmpeg_path.empty() ? "ffmpeg" : ffmpeg_path) +
                          " -hide_banner -loglevel warning" +
                          " -f matroska -i -" +
                          " -c copy" +
                          " -flvflags no_duration_filesize" +
                          " -f " + (url.rfind("srt://", 0) == 0 ? "mpegts" : "flv") +
                          " " + quote(url);

  // RTMP/FLV 对 HEVC 支持很差：Twitch / YouTube / B 站都只收 H.264。
  // 用 HEVC 推 RTMP 多半会被服务端拒掉，这里提前说清楚。
  if (url.rfind("rtmp", 0) == 0 && impl_->codec != Codec::H264) {
    DUO_WARN("RTMP 通常只接受 H.264，当前编码是 %s。"
             "建议加 --codec h264，或改用 SRT（支持 HEVC）",
             impl_->codec == Codec::Hevc ? "HEVC" : "AV1");
  }

  DUO_INFO("推流: %s", cmd.c_str());

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = read_end;
  si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

  std::vector<char> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back(static_cast<char>(0));

  const BOOL created = ::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                        &impl_->process);
  ::CloseHandle(read_end);  // 子进程已继承，父进程不再需要

  if (!created) {
    DUO_ERROR("启动 ffmpeg 失败: %lu", ::GetLastError());
    ::CloseHandle(write_end);
    return Status::IoError;
  }

  impl_->pipe = write_end;
  impl_->pipe_ready.store(true, std::memory_order_release);
  impl_->stop_requested.store(false, std::memory_order_release);

  const Status ms = impl_->muxer.open_sink(&Impl::on_mux_bytes, impl_.get(), impl_->codec,
                                           impl_->width, impl_->height, impl_->fps);
  if (!ok(ms)) {
    DUO_ERROR("推流封装器初始化失败: %s", to_string(ms));
    ::TerminateProcess(impl_->process.hProcess, 1);
    impl_->cleanup();
    return ms;
  }
  if (impl_->audio_enabled) {
    const Status as = impl_->muxer.enable_audio(impl_->audio_rate, impl_->audio_channels,
                                                impl_->audio_private);
    if (!ok(as)) {
      DUO_WARN("推流音轨启用失败: %s", to_string(as));
    }
  }

  impl_->running.store(true, std::memory_order_release);
  impl_->writer = std::thread([this] { impl_->writer_loop(); });

  DUO_INFO("推流已启动: %s", url.c_str());
  return Status::Ok;
}

Status StreamPusher::push_frame(ByteSpan frame, u64 capture_ts_ns, bool keyframe) {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return Status::NotInitialized;
  }

  // MkvMuxer 通过 sink 回调把字节吐进 pending 缓冲，写线程再喂管道。
  // 这样做的好处是 muxer 完全不知道管道存在，同一份封装逻辑既服务
  // 本地录制也服务推流。
  const Status st = impl_->muxer.write_frame(frame, capture_ts_ns, keyframe);
  if (ok(st)) {
    impl_->frames.fetch_add(1, std::memory_order_relaxed);
  } else {
    static std::atomic<int> errs{0};
    if (errs.fetch_add(1) < 3) {
      DUO_WARN("推流封装失败: %s", to_string(st));
    }
  }
  return st;
}

Status StreamPusher::stop() {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return Status::NotInitialized;
  }
  impl_->stop_requested.store(true, std::memory_order_release);
  if (impl_->writer.joinable()) {
    impl_->writer.join();
  }
  impl_->running.store(false, std::memory_order_release);

  // 关写端 → ffmpeg 收到 EOF → 正常收尾
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    ::FlushFileBuffers(impl_->pipe);
    ::CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
  }
  if (impl_->process.hProcess != nullptr) {
    ::WaitForSingleObject(impl_->process.hProcess, 5000);
  }
  impl_->cleanup();
  DUO_INFO("推流结束: %llu 帧, %.1f MB",
           static_cast<unsigned long long>(impl_->frames.load()),
           static_cast<double>(impl_->bytes.load()) / (1024.0 * 1024.0));
  return Status::Ok;
}

void StreamPusher::abort() {
  if (impl_->running.load(std::memory_order_acquire)) {
    impl_->stop_requested.store(true, std::memory_order_release);
    if (impl_->writer.joinable()) {
      impl_->writer.join();
    }
    impl_->running.store(false, std::memory_order_release);
  }
  if (impl_->process.hProcess != nullptr) {
    ::TerminateProcess(impl_->process.hProcess, 1);
  }
  impl_->cleanup();
}

bool StreamPusher::is_open() const noexcept {
  return impl_->running.load(std::memory_order_acquire);
}

u64 StreamPusher::frames_pushed() const noexcept {
  return impl_->frames.load(std::memory_order_relaxed);
}

u64 StreamPusher::bytes_pushed() const noexcept {
  return impl_->bytes.load(std::memory_order_relaxed);
}

}  // namespace duo

#else

namespace duo {

struct StreamPusher::Impl {
  // MkvMuxer 的 sink：把字节追加到 pending，由写线程消费
  static void on_mux_bytes(void* user, const u8* data, std::size_t size) {
    auto* self = static_cast<Impl*>(user);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pending.insert(self->pending.end(), data, data + size);
  }
};

StreamPusher::StreamPusher() : impl_(std::make_unique<Impl>()) {}
StreamPusher::~StreamPusher() = default;
Status StreamPusher::start(const std::string&, const std::string&) {
  return Status::NotSupported;
}
Status StreamPusher::push_frame(ByteSpan, u64, bool) { return Status::NotSupported; }
Status StreamPusher::stop() { return Status::NotSupported; }
void StreamPusher::abort() {}
bool StreamPusher::is_open() const noexcept { return false; }
u64 StreamPusher::frames_pushed() const noexcept { return 0; }
u64 StreamPusher::bytes_pushed() const noexcept { return 0; }
bool ffmpeg_available(const std::string&) { return false; }

}  // namespace duo

#endif
