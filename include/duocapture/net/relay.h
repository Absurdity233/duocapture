#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/net/transport.h"

namespace duo {

struct RelayClient {
  std::string host;
  u16 video_port = kDefaultVideoPort;
};

struct RelayConfig {
  u16 listen_video_port = kDefaultVideoPort;
  u16 listen_control_port = kDefaultControlPort;
  // The current control protocol carries no sender port; use this configured port.
  u16 sender_control_port = kDefaultControlPort;
  std::vector<RelayClient> clients;
};

struct RelayStats {
  u64 video_datagrams_received = 0;
  u64 video_datagrams_forwarded = 0;
  u64 control_datagrams_received = 0;
  u64 control_datagrams_forwarded = 0;
  u64 invalid_video_datagrams = 0;
  u64 invalid_control_datagrams = 0;
  u64 send_errors = 0;
};

// UDP forwarder for one upstream sender and a static set of downstream clients.
// Video packets are preserved byte-for-byte; receiver control messages are sent
// back to the latest upstream host learned from a valid video packet.
class RelayServer {
 public:
  RelayServer() = default;
  ~RelayServer();

  RelayServer(const RelayServer&) = delete;
  RelayServer& operator=(const RelayServer&) = delete;

  // background=true starts independent video/control receive loops. When false,
  // call run_once() from the owning event loop until stop() is requested.
  Status start(const RelayConfig& config, bool background = true);
  void stop();
  Status run_once(int timeout_ms = 0);

  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] RelayStats stats() const;
  [[nodiscard]] std::string learned_sender_host() const;

 private:
  void video_loop();
  void control_loop();
  Status forward_video_once(int timeout_ms);
  Status forward_control_once(int timeout_ms);

  RelayConfig config_;
  UdpSocket video_socket_;
  UdpSocket control_socket_;
  UdpSocket video_send_socket_;
  UdpSocket control_send_socket_;
  std::atomic<bool> running_{false};
  bool background_ = true;
  std::thread video_thread_;
  std::thread control_thread_;

  mutable std::mutex sender_mutex_;
  std::string sender_host_;
  mutable std::mutex stats_mutex_;
  RelayStats stats_{};
};

}  // namespace duo
