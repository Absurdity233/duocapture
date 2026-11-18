#include "duocapture/net/discovery.h"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace duo {
namespace {

constexpr u8 kProbeMagic[4] = {'D', 'U', 'O', 'D'};
constexpr u8 kReplyMagic[4] = {'D', 'U', 'O', 'A'};

void put_u16(Bytes& out, u16 v) {
  out.push_back(static_cast<u8>(v & 0xFF));
  out.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

void put_u32(Bytes& out, u32 v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
  }
}

u16 get_u16(const u8* p) {
  return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
}

u32 get_u32(const u8* p) {
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
         (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

}  // namespace

Bytes serialize_discovery_probe(const DiscoveryProbe& probe) {
  Bytes out;
  out.reserve(kDiscoveryProbeSize);
  out.insert(out.end(), kProbeMagic, kProbeMagic + 4);
  out.push_back(kDiscoveryVersion);
  put_u32(out, probe.nonce);
  put_u16(out, probe.ack_port);
  return out;
}

Bytes serialize_discovery_reply(const DiscoveryReply& reply) {
  Bytes out;
  out.reserve(kDiscoveryReplySize);
  out.insert(out.end(), kReplyMagic, kReplyMagic + 4);
  out.push_back(kDiscoveryVersion);
  put_u32(out, reply.nonce);
  put_u16(out, reply.video_port);
  return out;
}

bool parse_discovery_probe(ByteSpan data, DiscoveryProbe& out) {
  if (data.size() != kDiscoveryProbeSize) {
    return false;
  }
  if (std::memcmp(data.data(), kProbeMagic, 4) != 0) {
    return false;
  }
  if (data[4] != kDiscoveryVersion) {
    return false;
  }
  out.nonce = get_u32(data.data() + 5);
  out.ack_port = get_u16(data.data() + 9);
  return true;
}

bool parse_discovery_reply(ByteSpan data, DiscoveryReply& out) {
  if (data.size() != kDiscoveryReplySize) {
    return false;
  }
  if (std::memcmp(data.data(), kReplyMagic, 4) != 0) {
    return false;
  }
  if (data[4] != kDiscoveryVersion) {
    return false;
  }
  out.nonce = get_u32(data.data() + 5);
  out.video_port = get_u16(data.data() + 9);
  return true;
}

std::vector<std::string> local_broadcast_addresses() {
  std::vector<std::string> out;
#if defined(_WIN32)
  ULONG size = 16 * 1024;
  std::vector<u8> buffer(size);
  ULONG rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                    &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                             GAA_FLAG_SKIP_DNS_SERVER,
                                nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                &size);
  }
  if (rc != NO_ERROR) {
    return out;
  }

  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
       adapter = adapter->Next) {
    // 只考虑已启用的真实链路：APIPA（169.254）和断开的适配器广播不出去。
    if (adapter->OperStatus != IfOperStatusUp) {
      continue;
    }
    for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
      const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      if (addr == nullptr || addr->sin_family != AF_INET) {
        continue;
      }
      const u32 ip = ntohl(addr->sin_addr.s_addr);
      const u32 top = ip >> 24;
      if (top == 169 || top == 127) {
        continue;  // APIPA / 回环：广播出去没人收
      }
      // 前缀长度反推掩码；OnLinkPrefixLength 为 0 说明拿不到，跳过。
      const u8 prefix = unicast->OnLinkPrefixLength;
      if (prefix == 0 || prefix > 32) {
        continue;
      }
      const u32 mask = prefix == 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
      const u32 broadcast = (ip & mask) | ~mask;
      in_addr a{};
      a.s_addr = htonl(broadcast);
      char text[INET_ADDRSTRLEN] = {};
      if (::inet_ntop(AF_INET, &a, text, sizeof(text)) != nullptr) {
        out.emplace_back(text);
      }
    }
  }
#else
  struct ifaddrs* ifs = nullptr;
  if (::getifaddrs(&ifs) == 0) {
    for (auto* it = ifs; it != nullptr; it = it->ifa_next) {
      if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_BROADCAST) == 0) {
        continue;
      }
      const auto* bcast = reinterpret_cast<const sockaddr_in*>(it->ifa_broadaddr);
      if (bcast == nullptr) {
        continue;
      }
      char text[INET_ADDRSTRLEN] = {};
      if (::inet_ntop(AF_INET, &bcast->sin_addr, text, sizeof(text)) != nullptr) {
        out.emplace_back(text);
      }
    }
    ::freeifaddrs(ifs);
  }
#endif
  return out;
}

Status DiscoveryResponder::start(u16 discovery_port, u16 video_port) {
  video_port_ = video_port;

  const Status status = socket_.open("0.0.0.0", discovery_port, true);
  if (!ok(status)) {
    return status;
  }
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this] { loop(); });
  return Status::Ok;
}

void DiscoveryResponder::stop() {
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
  socket_.close();
}

DiscoveryResponder::~DiscoveryResponder() { stop(); }

void DiscoveryResponder::loop() {
  std::vector<u8> buffer(64);
  while (!stop_.load(std::memory_order_acquire)) {
    std::size_t received = 0;
    std::string from_host;
    u16 from_port = 0;
    const Status status = socket_.recv_from(
        MutByteSpan(buffer.data(), buffer.size()), received, from_host, from_port, 200);
    if (!ok(status) || received == 0) {
      continue;
    }

    DiscoveryProbe probe;
    if (!parse_discovery_probe(ByteSpan(buffer.data(), received), probe)) {
      continue;
    }

    void (*fn)(void*, const std::string&, u16) = nullptr;
    void* user = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sender_host_ = from_host;
      sender_ack_port_ = probe.ack_port;
      fn = probe_fn_;
      user = probe_user_;
    }
    if (fn != nullptr) {
      fn(user, from_host, probe.ack_port);
    }

    DiscoveryReply reply{};
    reply.nonce = probe.nonce;
    reply.video_port = video_port_;
    const Bytes wire = serialize_discovery_reply(reply);
    (void)socket_.send_to(ByteSpan(wire.data(), wire.size()), from_host, from_port);
  }
}

bool discover_receivers(u16 discovery_port, u16 ack_port, int timeout_ms,
                        std::vector<DiscoveryReply>& out, std::string& error) {
  UdpSocket socket;
  Status status = socket.open("0.0.0.0", 0, true);
  if (!ok(status)) {
    error = "打开发送发现包的套接字失败";
    return false;
  }
  status = socket.set_broadcast(true);
  if (!ok(status)) {
    error = "无法启用广播（set_broadcast）";
    return false;
  }

  // nonce 只用做自洽校验，不需要密码学强度。
  DiscoveryProbe probe{};
  probe.nonce = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                0x5EEDu;
  probe.ack_port = ack_port;
  const Bytes wire = serialize_discovery_probe(probe);

  std::vector<std::string> targets = local_broadcast_addresses();
  targets.emplace_back("255.255.255.255");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  auto next_probe = std::chrono::steady_clock::now();

  while (std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_probe) {
      for (const std::string& host : targets) {
        (void)socket.send_to(ByteSpan(wire.data(), wire.size()), host, discovery_port);
      }
      next_probe = now + std::chrono::milliseconds(300);
    }

    std::vector<u8> buffer(64);
    std::size_t received = 0;
    std::string from_host;
    u16 from_port = 0;
    status = socket.recv_from(MutByteSpan(buffer.data(), buffer.size()), received, from_host,
                              from_port, 100);
    if (!ok(status) || received == 0) {
      continue;
    }
    DiscoveryReply reply;
    if (!parse_discovery_reply(ByteSpan(buffer.data(), received), reply)) {
      continue;
    }
    if (reply.nonce != probe.nonce) {
      continue;  // 别人的迟到应答
    }
    reply.host = from_host;
    const auto duplicate = std::find_if(out.begin(), out.end(), [&](const DiscoveryReply& item) {
      return item.host == reply.host && item.video_port == reply.video_port;
    });
    if (duplicate == out.end()) out.push_back(std::move(reply));
  }

  // 把试过的地址列出来：最常见的原因就是接收端根本没启动，或者
  // Windows 防火墙拦了入站 UDP（默认拦，且不提示）。
  std::string tried;
  for (const std::string& t : targets) {
    if (!tried.empty()) {
      tried += ", ";
    }
    tried += t;
  }
  if (!out.empty()) return true;
  error = "在 " + std::to_string(timeout_ms / 1000) + " 秒内没有收到应答。\n" +
          "  已向这些地址广播探测（端口 " + std::to_string(discovery_port) + "）：" + tried +
          "\n" +
          "  请逐项确认：\n" +
          "   1. 接收端在跑，且是新版本（旧版本不应答探测）\n" +
          "   2. 接收端防火墙放行入站 UDP " + std::to_string(discovery_port) + "：\n" +
          "      netsh advfirewall firewall add rule name=DuoCapture-find " +
          "dir=in action=allow protocol=UDP localport=" + std::to_string(discovery_port) +
          "\n" +
          "   3. 两端在同一网段（不在同一 VLAN / 路由器隔离 / 客户端隔离）\n" +
          "  或者直接用 --target <接收端IP> 跳过发现。";
  return false;
}

bool discover_receiver(u16 discovery_port, u16 ack_port, int timeout_ms, DiscoveryReply& out,
                       std::string& error) {
  std::vector<DiscoveryReply> replies;
  if (!discover_receivers(discovery_port, ack_port, timeout_ms, replies, error)) return false;
  out = std::move(replies.front());
  return true;
}

bool DiscoveryResponder::last_sender(std::string& host, u16& ack_port) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sender_host_.empty()) {
    return false;
  }
  host = sender_host_;
  ack_port = sender_ack_port_;
  return true;
}

void DiscoveryResponder::set_probe_callback(void (*fn)(void* user, const std::string& host,
                                                       u16 ack_port),
                                            void* user) {
  std::lock_guard<std::mutex> lock(mutex_);
  probe_fn_ = fn;
  probe_user_ = user;
}

}  // namespace duo
