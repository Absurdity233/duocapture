#include "duocapture/net/relay.h"

#include <algorithm>
#include <array>

#include "duocapture/net/control_channel.h"
#include "duocapture/net/discovery.h"
#include "duocapture/net/packet.h"

namespace duo {
namespace {
constexpr int kPollMs = 50;
constexpr std::size_t kMaxDatagram = 2048;
}

RelayServer::~RelayServer() { stop(); }

Status RelayServer::start(const RelayConfig& config, bool background) {
  if (running_.load(std::memory_order_acquire) || config.clients.empty() ||
      config.listen_video_port == 0 || config.listen_control_port == 0 ||
      config.sender_control_port == 0) {
    return Status::InvalidArgument;
  }
  for (const auto& client : config.clients) {
    if (client.host.empty() || client.video_port == 0) {
      return Status::InvalidArgument;
    }
  }

  config_ = config;
  {
    std::lock_guard<std::mutex> lock(sender_mutex_);
    sender_host_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = {};
  }

  Status status = video_socket_.open("0.0.0.0", config_.listen_video_port);
  if (!ok(status)) return status;
  status = control_socket_.open("0.0.0.0", config_.listen_control_port);
  if (!ok(status)) {
    video_socket_.close();
    return status;
  }
  status = video_send_socket_.open("0.0.0.0", 0);
  if (!ok(status)) {
    control_socket_.close();
    video_socket_.close();
    return status;
  }
  status = control_send_socket_.open("0.0.0.0", 0);
  if (!ok(status)) {
    video_send_socket_.close();
    control_socket_.close();
    video_socket_.close();
    return status;
  }

  background_ = background;
  running_.store(true, std::memory_order_release);
  if (background_) {
    video_thread_ = std::thread([this] { video_loop(); });
    control_thread_ = std::thread([this] { control_loop(); });
  }
  return Status::Ok;
}

void RelayServer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  if (video_thread_.joinable()) video_thread_.join();
  if (control_thread_.joinable()) control_thread_.join();
  video_socket_.close();
  control_socket_.close();
  video_send_socket_.close();
  control_send_socket_.close();
}

Status RelayServer::run_once(int timeout_ms) {
  if (!running_.load(std::memory_order_acquire) || background_) {
    return Status::InvalidArgument;
  }
  const Status video = forward_video_once(timeout_ms);
  if (!running_.load(std::memory_order_acquire)) return Status::Ok;
  const Status control = forward_control_once(0);
  if (!ok(video) && video != Status::Timeout) return video;
  if (!ok(control) && control != Status::Timeout) return control;
  return Status::Ok;
}

RelayStats RelayServer::stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

std::string RelayServer::learned_sender_host() const {
  std::lock_guard<std::mutex> lock(sender_mutex_);
  return sender_host_;
}

void RelayServer::video_loop() {
  while (running_.load(std::memory_order_acquire)) {
    forward_video_once(kPollMs);
  }
}

void RelayServer::control_loop() {
  while (running_.load(std::memory_order_acquire)) {
    forward_control_once(kPollMs);
  }
}

Status RelayServer::forward_video_once(int timeout_ms) {
  std::array<u8, kMaxDatagram> buffer{};
  std::size_t received = 0;
  std::string from_host;
  u16 from_port = 0;
  const Status status = video_socket_.recv_from(MutByteSpan(buffer.data(), buffer.size()),
                                                received, from_host, from_port, timeout_ms);
  if (!ok(status)) return status;

  PacketHeader header{};
  ByteSpan payload;
  if (!parse(ByteSpan(buffer.data(), received), header, payload)) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.invalid_video_datagrams;
    return Status::Ok;
  }
  {
    std::lock_guard<std::mutex> lock(sender_mutex_);
    sender_host_ = from_host;
  }
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.video_datagrams_received;
  }

  const ByteSpan datagram(buffer.data(), received);
  for (const auto& client : config_.clients) {
    if (ok(video_send_socket_.send_to(datagram, client.host, client.video_port))) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.video_datagrams_forwarded;
    } else {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.send_errors;
    }
  }
  return Status::Ok;
}

Status RelayServer::forward_control_once(int timeout_ms) {
  std::array<u8, kMaxDatagram> buffer{};
  std::size_t received = 0;
  std::string from_host;
  u16 from_port = 0;
  const Status status = control_socket_.recv_from(MutByteSpan(buffer.data(), buffer.size()),
                                                  received, from_host, from_port, timeout_ms);
  if (!ok(status)) return status;

  ControlHeader header{};
  ByteSpan payload;
  if (!parse_control(ByteSpan(buffer.data(), received), header, payload)) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.invalid_control_datagrams;
    return Status::Ok;
  }
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.control_datagrams_received;
  }

  // Ignore control traffic from hosts that are not in the configured client set.
  const bool known_client = std::any_of(config_.clients.begin(), config_.clients.end(),
                                        [&from_host](const RelayClient& c) {
                                          return c.host == from_host;
                                        });
  if (!known_client) return Status::Ok;

  std::string sender;
  {
    std::lock_guard<std::mutex> lock(sender_mutex_);
    sender = sender_host_;
  }
  if (sender.empty()) return Status::Ok;

  const ByteSpan datagram(buffer.data(), received);
  if (ok(control_send_socket_.send_to(datagram, sender, config_.sender_control_port))) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.control_datagrams_forwarded;
  } else {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.send_errors;
  }
  return Status::Ok;
}

}  // namespace duo
