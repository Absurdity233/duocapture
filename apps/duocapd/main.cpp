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
#include "duocapture/pipeline/config_file.h"
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
      "  --config <文件>        从 JSON 配置加载（命令行参数覆盖文件里的值）\n"
      "  --target <IP>          目标主机（必填，除非用 --playback）\n"
      "  --port <端口>          目标端口，默认 %u\n"
      "  --control-port <端口>  控制通道端口，默认 %u\n"
      "  --display <序号>       显示器序号，默认 0\n"
      "  --fps <帧率>           默认 120\n"
      "  --bitrate <Mbps>       默认 80\n"
      "  --codec <hevc|h264|av1> 默认 hevc\n"
      "  --no-fec               关闭前向纠错\n"
      "  --fec-group <N>        FEC 组大小，2-64，默认 16\n"
      "  --retransmit <毫秒>    重传缓冲时长，默认 200\n"
      "  --gop <帧数>           IDR 间隔，0 = 无限（配合 intra refresh）\n"
      "  --no-intra-refresh     改用周期性 IDR\n"
      "  --pacer-mbps <Mbps>    限速器速率，0 = 按码率自动\n"
      "  --audio                采集系统声音\n"
      "  --audio-bitrate <kbps> 音频码率，默认 128\n"
      "  --record <文件.mkv>    零转码录制\n"
      "  --push <URL>           RTMP / SRT 推流地址\n"
      "  --ffmpeg <路径>        ffmpeg 路径，默认从 PATH 找\n"
      "  --playback <文件>      从裸流回放（不采集不编码）\n"
      "  --playback-fps <帧率>  回放帧率，默认 20\n"
      "  --ddapi                使用 DXGI Desktop Duplication 而非 WGC\n",
      kVersion, static_cast<unsigned>(duo::kDefaultVideoPort),
      static_cast<unsigned>(duo::kDefaultControlPort));
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

// 找出 --config 的值。返回值是参数下标，没找到返回 -1。
// 从 1 开始扫：--config 允许写在模式前面（duocapd --config x.json --stream）。
int find_config_arg(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0) {
      return (i + 1 < argc) ? i + 1 : -2;  // -2 = 写了 --config 却没给文件名
    }
  }
  return -1;
}

int run_stream(int argc, char** argv) {
  duo::SenderConfig config;
  config.target_host.clear();

  // 先加载配置文件，再让命令行逐项覆盖：命令行是更具体的意图，
  // 临时调参不该逼用户去改文件。
  const int config_arg = find_config_arg(argc, argv);
  if (config_arg == -2) {
    std::printf("--config 需要一个文件路径\n");
    return 1;
  }
  if (config_arg > 0) {
    std::string error;
    const duo::Status status =
        duo::apply_sender_config_file(argv[config_arg], config, error);
    if (!duo::ok(status)) {
      std::printf("读取配置 %s 失败:\n%s", argv[config_arg], error.c_str());
      return 1;
    }
    std::printf("已加载配置 %s\n", argv[config_arg]);
  }

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = i + 1 < argc;

    if (arg == "--config" && has_next) {
      ++i;  // 上面已经处理过
    } else if (arg == "--target" && has_next) {
      config.target_host = argv[++i];
    } else if (arg == "--port" && has_next) {
      config.target_port = static_cast<duo::u16>(std::atoi(argv[++i]));
    } else if (arg == "--control-port" && has_next) {
      config.control_port = static_cast<duo::u16>(std::atoi(argv[++i]));
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
      } else if (codec == "hevc") {
        config.codec = duo::Codec::Hevc;
      } else {
        std::printf("未知编码 %s，可选 hevc / h264 / av1\n", codec.c_str());
        return 1;
      }
    } else if (arg == "--no-fec") {
      config.enable_fec = false;
    } else if (arg == "--fec-group" && has_next) {
      config.fec_group_size = static_cast<std::size_t>(std::atoi(argv[++i]));
    } else if (arg == "--retransmit" && has_next) {
      config.retransmit_window_ms = static_cast<duo::u32>(std::atoi(argv[++i]));
    } else if (arg == "--gop" && has_next) {
      config.gop_frames = static_cast<duo::u32>(std::atoi(argv[++i]));
    } else if (arg == "--pacer-mbps" && has_next) {
      // 0 = 交回自动逻辑（按码率留余量）。
      const double mbps = std::atof(argv[++i]);
      config.pacer_bytes_per_second =
          mbps > 0.0 ? static_cast<duo::u64>(mbps * 1'000'000.0 / 8.0) : 0;
    } else if (arg == "--no-intra-refresh") {
      config.intra_refresh = false;
    } else if (arg == "--record" && has_next) {
      config.record_path = argv[++i];
    } else if (arg == "--playback" && has_next) {
      config.playback_path = argv[++i];
    } else if (arg == "--playback-fps" && has_next) {
      config.playback_fps = static_cast<duo::u32>(std::atoi(argv[++i]));
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
    } else {
      // 拼错的参数静默忽略是最浪费时间的一类问题：写错 --bitrat 会
      // 悄悄按默认 80 Mbps 跑，画质不对也找不到原因。
      std::printf("警告: 未知参数 %s（用 duocapd 不带参数查看用法）\n", arg.c_str());
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

  // 允许把 --config 写在模式前面（duocapd --config x.json --stream ...），
  // 这是写配置文件的人更自然的顺序。
  int mode_index = 1;
  while (mode_index + 1 < argc && std::strcmp(argv[mode_index], "--config") == 0) {
    mode_index += 2;
  }
  if (mode_index >= argc) {
    print_usage();
    return 1;
  }

  const std::string mode = argv[mode_index];

  if (mode == "--list-displays") {
    return run_list_displays();
  }
  if (mode == "--selftest") {
    const bool pass = duo::run_net_selftest(true);
    std::printf("%s\n", pass ? "selftest PASSED" : "selftest FAILED");
    return pass ? 0 : 1;
  }
  if (mode == "--probe") {
    const double seconds = mode_index + 1 < argc ? std::atof(argv[mode_index + 1]) : 5.0;
    return run_probe(seconds > 0.0 ? seconds : 5.0);
  }
  if (mode == "--stream") {
    return run_stream(argc, argv);
  }

  print_usage();
  return 1;
}
