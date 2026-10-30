// duocapd —— B 端（被采集机）程序。
//
// 用法:
//   duocapd --list-displays
//   duocapd --selftest
//   duocapd --probe [秒数]
//   duocapd --stream --target <主机> [选项]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "duocapture/capture/capture_source.h"
#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "duocapture/pipeline/selftest.h"
#include "duocapture/pipeline/sender.h"

namespace {

constexpr const char* kVersion = "0.2.0";

void print_usage() {
  std::printf(
      "duocapd %s\n"
      "用法:\n"
      "  duocapd --list-displays                列出可采集的显示器\n"
      "  duocapd --selftest                     运行网络层自检\n"
      "  duocapd --probe [秒数]                 采集探测，默认 5 秒\n"
      "  duocapd --stream --target <主机> [选项] 推流\n"
      "\n"
      "推流选项:\n"
      "  --port <端口>          目标端口，默认 %u\n"
      "  --display <序号>       显示器序号，默认 0\n"
      "  --fps <帧率>           默认 120\n"
      "  --bitrate <Mbps>       默认 80\n"
      "  --codec <hevc|h264|av1> 默认 hevc\n"
      "  --no-fec               关闭前向纠错\n"
      "  --ddapi                使用 DXGI Desktop Duplication 而非 WGC\n",
      kVersion, static_cast<unsigned>(duo::kDefaultVideoPort));
}

int run_list_displays() {
  const auto displays = duo::list_displays();
  if (displays.empty()) {
    std::printf("未发现可采集的显示器\n");
    return 1;
  }
  for (std::size_t i = 0; i < displays.size(); ++i) {
    std::printf("[%zu] %s\n", i, displays[i].c_str());
  }
  return 0;
}

int run_probe(double seconds) {
  auto source = duo::create_capture_source(true);
  if (!source) {
    std::printf("当前平台没有可用的采集后端\n");
    return 1;
  }

  duo::Status status = source->start(0);
  if (!duo::ok(status)) {
    std::printf("启动采集失败: %s\n", duo::to_string(status));
    return 1;
  }

  std::printf("采集后端: %s，运行 %.1f 秒\n", source->backend_name(), seconds);

  const duo::u64 begin = duo::Clock::now_ns();
  const duo::u64 deadline = begin + static_cast<duo::u64>(seconds * 1e9);

  duo::u64 frames = 0;
  duo::u64 timeouts = 0;
  duo::u64 errors = 0;
  duo::u32 last_width = 0;
  duo::u32 last_height = 0;

  while (duo::Clock::now_ns() < deadline) {
    duo::FrameView view;
    status = source->acquire(view, 200);

    if (status == duo::Status::Timeout) {
      ++timeouts;
      continue;
    }
    if (status == duo::Status::Closed) {
      std::printf("采集会话失效，重新初始化\n");
      source->stop();
      if (!duo::ok(source->start(0))) {
        std::printf("重新初始化失败\n");
        return 1;
      }
      continue;
    }
    if (!duo::ok(status)) {
      ++errors;
      std::printf("采集出错: %s\n", duo::to_string(status));
      break;
    }

    ++frames;
    last_width = view.width;
    last_height = view.height;
    source->release(view);
  }

  source->stop();

  const double elapsed_s = duo::Clock::to_ms(duo::Clock::now_ns() - begin) / 1000.0;
  const double fps = elapsed_s > 0.0 ? static_cast<double>(frames) / elapsed_s : 0.0;

  std::printf("帧数=%llu  时长=%.2fs  帧率=%.1f fps  尺寸=%ux%u  超时=%llu  错误=%llu\n",
              static_cast<unsigned long long>(frames), elapsed_s, fps, last_width, last_height,
              static_cast<unsigned long long>(timeouts), static_cast<unsigned long long>(errors));

  return frames > 0 ? 0 : 1;
}

int run_stream(int argc, char** argv) {
  duo::SenderConfig config;
  config.target_host.clear();

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = i + 1 < argc;

    if (arg == "--target" && has_next) {
      config.target_host = argv[++i];
    } else if (arg == "--port" && has_next) {
      config.target_port = static_cast<duo::u16>(std::atoi(argv[++i]));
    } else if (arg == "--display" && has_next) {
      config.display_index = static_cast<duo::u32>(std::atoi(argv[++i]));
    } else if (arg == "--fps" && has_next) {
      config.fps = static_cast<duo::u32>(std::atoi(argv[++i]));
    } else if (arg == "--bitrate" && has_next) {
      config.bitrate_bps = static_cast<duo::u32>(std::atof(argv[++i]) * 1'000'000);
    } else if (arg == "--codec" && has_next) {
      const std::string codec = argv[++i];
      if (codec == "h264") {
        config.codec = duo::Codec::H264;
      } else if (codec == "av1") {
        config.codec = duo::Codec::Av1;
      } else {
        config.codec = duo::Codec::Hevc;
      }
    } else if (arg == "--no-fec") {
      config.enable_fec = false;
    } else if (arg == "--no-intra-refresh") {
      config.intra_refresh = false;
    } else if (arg == "--record" && has_next) {
      config.record_path = argv[++i];
    } else if (arg == "--playback" && has_next) {
      config.playback_path = argv[++i];
    } else if (arg == "--push" && has_next) {
      config.push_url = argv[++i];
    } else if (arg == "--ffmpeg" && has_next) {
      config.ffmpeg_path = argv[++i];
    } else if (arg == "--audio") {
      config.capture_audio = true;
    } else if (arg == "--audio-bitrate" && has_next) {
      config.audio_bitrate_bps = static_cast<duo::u32>(std::atof(argv[++i]) * 1000);
    } else if (arg == "--ddapi") {
      config.prefer_wgc = false;
    }
  }

  if (config.target_host.empty() && config.playback_path.empty()) {
    std::printf("缺少 --target <主机>（或 --playback <文件>）\n");
    return 1;
  }

  duo::Sender sender;
  duo::Status status = sender.start(config);
  if (!duo::ok(status)) {
    std::printf("启动发送端失败: %s\n", duo::to_string(status));
    return 1;
  }

  std::printf("推流中，Ctrl+C 停止\n");
  duo::u64 prev_bytes = 0;
  duo::u64 prev_packets = 0;
  auto prev_time = std::chrono::steady_clock::now();
  while (sender.running()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const duo::SenderStats stats = sender.stats();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - prev_time).count();
    // 码率必须用差分算。累计字节直接乘 8 得到的是"累计兆比特"，
    // 看起来像码率飙升，其实只是累计量在增长。
    const double mbps =
        dt > 0.0 ? static_cast<double>(stats.bytes_sent - prev_bytes) * 8.0 / 1e6 / dt : 0.0;
    const double pps = dt > 0.0 ? static_cast<double>(stats.packets_sent - prev_packets) / dt : 0.0;
    std::printf("采集 %llu  编码 %llu  发送 %llu  包/s %.0f  码率 %.1f Mbps  编码 %.2f ms  错误 %llu\n",
                static_cast<unsigned long long>(stats.frames_captured),
                static_cast<unsigned long long>(stats.frames_encoded),
                static_cast<unsigned long long>(stats.frames_sent), pps, mbps,
                stats.avg_encode_ms, static_cast<unsigned long long>(stats.encode_errors));
    prev_bytes = stats.bytes_sent;
    prev_packets = stats.packets_sent;
    prev_time = now;
  }

  sender.stop();
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

  if (mode == "--list-displays") {
    return run_list_displays();
  }
  if (mode == "--selftest") {
    const bool pass = duo::run_net_selftest(true);
    std::printf("%s\n", pass ? "selftest PASSED" : "selftest FAILED");
    return pass ? 0 : 1;
  }
  if (mode == "--probe") {
    const double seconds = argc >= 3 ? std::atof(argv[2]) : 5.0;
    return run_probe(seconds > 0.0 ? seconds : 5.0);
  }
  if (mode == "--stream") {
    return run_stream(argc, argv);
  }

  print_usage();
  return 1;
}
