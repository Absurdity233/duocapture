#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/net/relay.h"

namespace {
void usage() {
  std::printf("用法: duorelay --client HOST:PORT [--client HOST:PORT ...] [选项]\n"
              "  --video-port PORT    中转视频端口，默认 5000\n"
              "  --control-port PORT  中转控制端口，默认 5001\n"
              "  --sender-port PORT   上游发送端控制端口，默认 5001\n");
}

bool endpoint(const std::string& text, duo::RelayClient& out) {
  const auto split = text.rfind(':');
  if (split == std::string::npos || split == 0 || split + 1 >= text.size()) return false;
  out.host = text.substr(0, split);
  const long port = std::strtol(text.c_str() + split + 1, nullptr, 10);
  if (port < 1 || port > 65535) return false;
  out.video_port = static_cast<duo::u16>(port);
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  duo::RelayConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--client" && i + 1 < argc) {
      duo::RelayClient client;
      if (!endpoint(argv[++i], client)) {
        std::printf("非法客户端地址: %s\n", argv[i]);
        return 2;
      }
      config.clients.push_back(std::move(client));
    } else if (arg == "--video-port" && i + 1 < argc) {
      config.listen_video_port = static_cast<duo::u16>(std::atoi(argv[++i]));
    } else if (arg == "--control-port" && i + 1 < argc) {
      config.listen_control_port = static_cast<duo::u16>(std::atoi(argv[++i]));
    } else if (arg == "--sender-port" && i + 1 < argc) {
      config.sender_control_port = static_cast<duo::u16>(std::atoi(argv[++i]));
    } else {
      usage();
      return 2;
    }
  }
  if (config.clients.empty()) {
    usage();
    return 2;
  }
  duo::RelayServer relay;
  const duo::Status status = relay.start(config);
  if (!duo::ok(status)) {
    std::printf("启动中转失败: %s\n", duo::to_string(status));
    return 1;
  }
  std::printf("中转运行中: video=%u control=%u clients=%zu\n",
              static_cast<unsigned>(config.listen_video_port),
              static_cast<unsigned>(config.listen_control_port), config.clients.size());
  for (;;) std::this_thread::sleep_for(std::chrono::hours(24));
}
