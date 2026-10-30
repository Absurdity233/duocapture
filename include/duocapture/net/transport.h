#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"

namespace duo {

// 非阻塞 UDP 套接字封装。Windows 与 POSIX 通用。
class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;

  // bind_host 为空表示绑定所有接口；port 为 0 表示由系统分配。
  Status open(const std::string& bind_host, u16 port, bool reuse_addr = true);
  void close();
  [[nodiscard]] bool is_open() const noexcept;

  Status set_recv_buffer(std::size_t bytes);
  Status set_send_buffer(std::size_t bytes);
  Status set_nonblocking(bool enabled);

  Status send_to(ByteSpan data, const std::string& host, u16 port);

  // timeout_ms < 0 表示阻塞等待。
  Status recv_from(MutByteSpan buffer, std::size_t& received, std::string& from_host,
                   u16& from_port, int timeout_ms);

  [[nodiscard]] u16 local_port() const;
  [[nodiscard]] u64 bytes_sent() const noexcept { return bytes_sent_; }
  [[nodiscard]] u64 bytes_received() const noexcept { return bytes_received_; }

 private:
  std::intptr_t handle_ = -1;
  u64 bytes_sent_ = 0;
  u64 bytes_received_ = 0;
};

}  // namespace duo
