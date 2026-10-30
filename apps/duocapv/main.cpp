// duocapv —— A 端（接收机）程序。
//
// 用法:
//   duocapv --selftest
//   duocapv --monitor [端口] [秒数]
//   duocapv --play [选项]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "duocapture/common/types.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/packet.h"
#include "duocapture/net/transport.h"
#include "duocapture/pipeline/receiver.h"
#include "duocapture/pipeline/selftest.h"

namespace {

constexpr const char* kVersion = "0.2.0";

void print_usage() {
  std::printf(
      "duocapv %s\n"
      "用法:\n"
      "  duocapv --selftest               运行网络层自检\n"
      "  duocapv --monitor [端口] [秒数]  监听 UDP 并打印统计\n"
      "  duocapv --play [选项]            接收并呈现\n"
      "\n"
      "播放选项:\n"
      "  --port <端口>       监听端口，默认 %u\n"
      "  --width <像素>      窗口宽，默认跟随码流\n"
      "  --height <像素>     窗口高，默认跟随码流\n"
      "  --no-window         无窗口，只解码不显示（性能测试用）\n",
      kVersion, static_cast<unsigned>(duo::kDefaultVideoPort));
}

int run_monitor(duo::u16 port, double seconds) {
  duo::UdpSocket socket;
  duo::Status status = socket.open("0.0.0.0", port);
  if (!duo::ok(status)) {
    std::printf("绑定端口 %u 失败: %s\n", static_cast<unsigned>(port), duo::to_string(status));
    return 1;
  }
  socket.set_recv_buffer(16u * 1024u * 1024u);

  std::printf("监听 0.0.0.0:%u，运行 %.1f 秒\n", static_cast<unsigned>(socket.local_port()),
              seconds);

  duo::JitterBuffer jitter;
  std::vector<duo::u8> buffer(2048);

  const duo::u64 begin = duo::Clock::now_ns();
  const duo::u64 deadline = begin + static_cast<duo::u64>(seconds * 1e9);

  duo::u64 datagrams = 0;
  duo::u64 malformed = 0;
  duo::u64 last_report = begin;
  duo::u64 window_datagrams = 0;

  while (duo::Clock::now_ns() < deadline) {
    std::size_t received = 0;
    std::string from_host;
    duo::u16 from_port = 0;

    status = socket.recv_from(buffer, received, from_host, from_port, 200);
    if (status == duo::Status::Timeout) {
      continue;
    }
    if (!duo::ok(status)) {
      std::printf("接收错误: %s\n", duo::to_string(status));
      break;
    }

    ++datagrams;
    ++window_datagrams;

    duo::PacketHeader header{};
    duo::ByteSpan payload;
    if (!duo::parse(duo::ByteSpan(buffer.data(), received), header, payload)) {
      ++malformed;
      continue;
    }

    duo::Packet packet;
    packet.header = header;
    packet.payload.assign(payload.begin(), payload.end());
    jitter.insert(std::move(packet));

    // monitor 模式也要消费，否则窗口会被填满、组装统计永远是 0。
    duo::AssembledFrame assembled;
    while (jitter.try_pop(assembled)) {
    }

    const duo::u64 now = duo::Clock::now_ns();
    if (now - last_report >= 1'000'000'000ull) {
      std::printf("[%s:%u] 包=%llu/s  累计=%llu  帧组装=%llu  FEC恢复=%llu  待组装帧=%zu\n",
                  from_host.c_str(), static_cast<unsigned>(from_port),
                  static_cast<unsigned long long>(window_datagrams),
                  static_cast<unsigned long long>(datagrams),
                  static_cast<unsigned long long>(jitter.stats().frames_assembled),
                  static_cast<unsigned long long>(jitter.stats().packets_recovered),
                  jitter.pending_frames());
      last_report = now;
      window_datagrams = 0;
    }
  }

  const double elapsed_s = duo::Clock::to_ms(duo::Clock::now_ns() - begin) / 1000.0;
  std::printf(
      "结束: 收到 %llu 个包，%llu 个格式错误，平均 %.0f 包/秒，组装 %llu 帧，FEC 恢复 %llu 个包\n",
      static_cast<unsigned long long>(datagrams), static_cast<unsigned long long>(malformed),
      elapsed_s > 0.0 ? static_cast<double>(datagrams) / elapsed_s : 0.0,
      static_cast<unsigned long long>(jitter.stats().frames_assembled),
      static_cast<unsigned long long>(jitter.stats().packets_recovered));

  return datagrams > 0 ? 0 : 1;
}

// 示例分析回调：统计纹理的亮度均值。真实场景会换成
// TensorRT / DirectML 推理。这里故意做一点计算，模拟 CV 负载。
void sample_analyze(void* user, const duo::DecodedFrame& frame) {
  auto* counter = static_cast<std::atomic<duo::u64>*>(user);
  if (counter != nullptr) {
    counter->fetch_add(1, std::memory_order_relaxed);
  }
  if (frame.texture == nullptr) {
    return;
  }
  // 模拟 CV 处理开销。默认 1ms（轻负载，跟得上 20fps）；
  // 环境变量 DUO_ANALYZE_MS 可以调大，用来验证「忙则丢帧」语义。
  static const int analyze_ms = [] {
    const char* env = std::getenv("DUO_ANALYZE_MS");
    return env != nullptr ? std::atoi(env) : 1;
  }();
  if (analyze_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(analyze_ms));
  }
}

int run_play(int argc, char** argv) {
  duo::ReceiverConfig config;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = i + 1 < argc;

    if (arg == "--port" && has_next) {
      config.listen_port = static_cast<duo::u16>(std::atoi(argv[++i]));
    } else if (arg == "--width" && has_next) {
      config.width = static_cast<duo::u32>(std::atoi(argv[++i]));
    } else if (arg == "--height" && has_next) {
      config.height = static_cast<duo::u32>(std::atoi(argv[++i]));
    } else if (arg == "--no-window") {
      config.show_window = false;
    } else if (arg == "--analyze") {
      config.enable_analysis = true;
    }
  }

  duo::Receiver receiver;
  std::atomic<duo::u64> analyzed{0};
  if (config.enable_analysis) {
    receiver.set_analysis_callback(&sample_analyze, &analyzed);
  }
  duo::Status status = receiver.start(config);
  if (!duo::ok(status)) {
    std::printf("启动接收端失败: %s\n", duo::to_string(status));
    return 1;
  }

  std::printf("接收中，关闭窗口或 Ctrl+C 停止\n");

  auto last_report = std::chrono::steady_clock::now();
  while (receiver.running()) {
    if (config.show_window && !receiver.pump()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));

    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(1)) {
      const duo::ReceiverStats stats = receiver.stats();
      std::printf("收包 %llu  组装 %llu  呈现 %llu  丢帧 %llu  解码错误 %llu  %.1f fps  "
                  "端到端 p50 %.1f / p99 %.1f ms  分析 %llu(丢 %llu)\n",
                  static_cast<unsigned long long>(stats.datagrams_received),
                  static_cast<unsigned long long>(stats.frames_assembled),
                  static_cast<unsigned long long>(stats.frames_presented),
                  static_cast<unsigned long long>(stats.frames_dropped),
                  static_cast<unsigned long long>(stats.decoder_errors), stats.receive_fps,
                  stats.p50_end_to_end_ms, stats.p99_end_to_end_ms,
                  static_cast<unsigned long long>(stats.frames_analyzed),
                  static_cast<unsigned long long>(stats.frames_analysis_dropped));
      last_report = now;
    }
  }

  receiver.stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  duo::set_log_level(duo::LogLevel::Info);

  if (argc < 2) {
    print_usage();
    return 1;
  }

  const std::string mode = argv[1];

  if (mode == "--selftest") {
    const bool pass = duo::run_net_selftest(true);
    std::printf("%s\n", pass ? "selftest PASSED" : "selftest FAILED");
    return pass ? 0 : 1;
  }
  if (mode == "--monitor") {
    const duo::u16 port =
        argc >= 3 ? static_cast<duo::u16>(std::atoi(argv[2])) : duo::kDefaultVideoPort;
    const double seconds = argc >= 4 ? std::atof(argv[3]) : 10.0;
    return run_monitor(port, seconds > 0.0 ? seconds : 10.0);
  }
  if (mode == "--play") {
    return run_play(argc, argv);
  }

  print_usage();
  return 1;
}
