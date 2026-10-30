// duocapv —— A 端（接收机）程序。
//
// 当前可用：网络层自检、UDP 监听统计。
// 解码与呈现管线在 M1 落地。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "duocapture/common/clock.h"
#include "duocapture/common/log.h"
#include "duocapture/common/types.h"
#include "duocapture/net/jitter_buffer.h"
#include "duocapture/net/packet.h"
#include "duocapture/net/transport.h"
#include "duocapture/pipeline/selftest.h"

namespace {

void print_usage() {
  std::printf(
      "duocapv %s\n"
      "用法:\n"
      "  duocapv --selftest               运行网络层自检\n"
      "  duocapv --monitor [端口] [秒数]  监听 UDP 并打印统计（默认 5000 端口 / 10 秒）\n"
      "  duocapv --play <主机>            连接并呈现（M1 实现）\n",
      "0.1.0");
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
    std::printf("解码与呈现管线将在 M1 实现（NVDEC → 交换链）。\n");
    return 1;
  }

  print_usage();
  return 1;
}
