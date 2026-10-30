#include "duocapture/net/transport.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace duo {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

void ensure_winsock() {
  static const int result = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data);
  }();
  (void)result;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void ensure_winsock() {}
#endif

NativeSocket to_native(std::intptr_t handle) noexcept {
  return static_cast<NativeSocket>(handle);
}

std::intptr_t to_handle(NativeSocket socket) noexcept { return static_cast<std::intptr_t>(socket); }

}  // namespace

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_), bytes_sent_(other.bytes_sent_), bytes_received_(other.bytes_received_) {
  other.handle_ = -1;
  other.bytes_sent_ = 0;
  other.bytes_received_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    bytes_sent_ = other.bytes_sent_;
    bytes_received_ = other.bytes_received_;
    other.handle_ = -1;
    other.bytes_sent_ = 0;
    other.bytes_received_ = 0;
  }
  return *this;
}

Status UdpSocket::open(const std::string& bind_host, u16 port, bool reuse_addr) {
  ensure_winsock();
  close();

  NativeSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket == kInvalidSocket) {
    return Status::IoError;
  }

  if (reuse_addr) {
    int yes = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                 sizeof(yes));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (bind_host.empty()) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status::InvalidArgument;
  }

  if (::bind(socket, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status::IoError;
  }

  handle_ = to_handle(socket);
  return Status::Ok;
}

void UdpSocket::close() {
  if (handle_ == -1) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(to_native(handle_));
#else
  ::close(to_native(handle_));
#endif
  handle_ = -1;
}

bool UdpSocket::is_open() const noexcept { return handle_ != -1; }

Status UdpSocket::set_recv_buffer(std::size_t bytes) {
  if (!is_open()) {
    return Status::NotInitialized;
  }
  int size = static_cast<int>(bytes);
  const int rc = ::setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVBUF,
                              reinterpret_cast<const char*>(&size), sizeof(size));
  return rc == 0 ? Status::Ok : Status::IoError;
}

Status UdpSocket::set_send_buffer(std::size_t bytes) {
  if (!is_open()) {
    return Status::NotInitialized;
  }
  int size = static_cast<int>(bytes);
  const int rc = ::setsockopt(to_native(handle_), SOL_SOCKET, SO_SNDBUF,
                              reinterpret_cast<const char*>(&size), sizeof(size));
  return rc == 0 ? Status::Ok : Status::IoError;
}

Status UdpSocket::set_nonblocking(bool enabled) {
  if (!is_open()) {
    return Status::NotInitialized;
  }
#if defined(_WIN32)
  u_long mode = enabled ? 1 : 0;
  return ::ioctlsocket(to_native(handle_), FIONBIO, &mode) == 0 ? Status::Ok : Status::IoError;
#else
  const int flags = ::fcntl(to_native(handle_), F_GETFL, 0);
  if (flags < 0) {
    return Status::IoError;
  }
  const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(to_native(handle_), F_SETFL, updated) == 0 ? Status::Ok : Status::IoError;
#endif
}

Status UdpSocket::send_to(ByteSpan data, const std::string& host, u16 port) {
  if (!is_open()) {
    return Status::NotInitialized;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    return Status::InvalidArgument;
  }

  const int sent = ::sendto(to_native(handle_), reinterpret_cast<const char*>(data.data()),
                            static_cast<int>(data.size()), 0,
                            reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (sent < 0) {
    return Status::IoError;
  }
  bytes_sent_ += static_cast<u64>(sent);
  return Status::Ok;
}

Status UdpSocket::recv_from(MutByteSpan buffer, std::size_t& received, std::string& from_host,
                            u16& from_port, int timeout_ms) {
  if (!is_open()) {
    return Status::NotInitialized;
  }

  if (timeout_ms >= 0) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(to_native(handle_), &read_set);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = ::select(static_cast<int>(handle_ + 1), &read_set, nullptr, nullptr, &tv);
    if (ready == 0) {
      return Status::Timeout;
    }
    if (ready < 0) {
      return Status::IoError;
    }
  }

  sockaddr_in from{};
#if defined(_WIN32)
  int from_len = sizeof(from);
#else
  socklen_t from_len = sizeof(from);
#endif

  const int got = ::recvfrom(to_native(handle_), reinterpret_cast<char*>(buffer.data()),
                             static_cast<int>(buffer.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
  if (got < 0) {
    return Status::IoError;
  }

  received = static_cast<std::size_t>(got);
  bytes_received_ += static_cast<u64>(got);

  char text[INET_ADDRSTRLEN]{};
  ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
  from_host = text;
  from_port = ntohs(from.sin_port);
  return Status::Ok;
}

u16 UdpSocket::local_port() const {
  if (!is_open()) {
    return 0;
  }
  sockaddr_in addr{};
#if defined(_WIN32)
  int len = sizeof(addr);
#else
  socklen_t len = sizeof(addr);
#endif
  if (::getsockname(to_native(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return 0;
  }
  return ntohs(addr.sin_port);
}

}  // namespace duo
