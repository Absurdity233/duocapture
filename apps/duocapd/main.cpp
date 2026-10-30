// duocapd —— B 端（被采集机）程序。
//
// 当前可用：显示器枚举、网络层自检、采集探测。
// 完整推流管线（采集 → NVENC → UDP）在 M1 落地。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "duocapture/capture/capture_source.h"
#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "duocapture/pipeline/selftest.h"

namespace {

void print_usage() {
  std::printf(
      "duocapd %s\n"
      "用法:\n"
      "  duocapd --list-displays          列出可采集的显示器\n"
      "  duocapd --selftest               运行网络层自检\n"
      "  duocapd --probe [秒数]           采集探测，默认 5 秒\n"
      "  duocapd --stream [选项]          启动推流（M1 实现）\n",
      "0.1.0");
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
  auto source = duo::create_capture_source();
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
    std::printf("推流管线将在 M1 实现（采集 → NVENC → UDP）。\n");
    return 1;
  }

  print_usage();
  return 1;
}
