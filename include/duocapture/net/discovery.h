#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/net/transport.h"

namespace duo {

// 局域网点对点自动发现。
//
// 解决的问题：现在必须在一端填另一端的 IP。同一网段下这纯属多余——
// 发送端广播一声「谁在收」，接收端应一声「我在，端口是这些」，
// 发送端就知道往哪发了。
//
// 协议（全部小端），走 UDP：
//
//   探测  DUOD | ver(1) | nonce(4) | ack_port(2)   -> 广播
//   应答  DUOA | ver(1) | nonce(4) | video_port(2) -> 单播回探测方
//
// ack_port 是发送端本地的控制端口——接收端要靠它回发 NACK / 请求关键帧。
// 把它放在探测包里，接收端一次交互就同时学到「往哪发视频」和
// 「往哪发控制」，两端都不需要手填任何地址。
//
// nonce 原样回传，用来在同网段有多个接收端时确认这是对本次探测的应答，
// 而不是别的程序留下的迟到包。
//
// 只在局域网用：广播不出网段，也不需要任何鉴权——发现只是「找到人」，
// 真正的数据流仍然是点对点的。任何收到广播的主机都能应答，所以这个
// 协议不能用来做信任判断。
inline constexpr u16 kDefaultDiscoveryPort = 50002;
inline constexpr std::size_t kDiscoveryProbeSize = 11;
inline constexpr std::size_t kDiscoveryReplySize = 11;
inline constexpr u8 kDiscoveryVersion = 1;

struct DiscoveryProbe {
  u32 nonce = 0;
  u16 ack_port = kDefaultControlPort;  // 发送端接收 NACK 的端口
  std::string host;                    // 由 recv_from 填，不参与序列化
};

struct DiscoveryReply {
  u32 nonce = 0;
  u16 video_port = kDefaultVideoPort;  // 接收端监听视频的端口
  std::string host;                    // 由 recv_from 填，不参与序列化
};

Bytes serialize_discovery_probe(const DiscoveryProbe& probe);
Bytes serialize_discovery_reply(const DiscoveryReply& reply);

// 严格校验长度与 magic：收到什么垃圾都不能当作有效应答。
bool parse_discovery_probe(ByteSpan data, DiscoveryProbe& out);
bool parse_discovery_reply(ByteSpan data, DiscoveryReply& out);

// 接收端：在发现端口上应答探测。一个后台线程，成本可以忽略。
class DiscoveryResponder {
 public:
  DiscoveryResponder() = default;
  ~DiscoveryResponder();

  DiscoveryResponder(const DiscoveryResponder&) = delete;
  DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

  // video_port 是往应答里填的值（本机监听视频的端口）。
  Status start(u16 discovery_port, u16 video_port);

  // 最近一次探测到的发送端（IP + 它的控制端口）。没有探测过则返回 false。
  bool last_sender(std::string& host, u16& ack_port) const;

  // 收到探测时回调（在发现线程里调用），用来立刻学习发送端地址。
  void set_probe_callback(void (*fn)(void* user, const std::string& host, u16 ack_port),
                          void* user);
  void stop();

 private:
  void loop();

  UdpSocket socket_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  u16 video_port_ = kDefaultVideoPort;

  mutable std::mutex mutex_;
  std::string sender_host_;
  u16 sender_ack_port_ = kDefaultControlPort;
  void (*probe_fn_)(void*, const std::string&, u16) = nullptr;
  void* probe_user_ = nullptr;
};

// 发送端：广播探测并等应答。超时返回 false 并填 error。
//
// 会同时往 255.255.255.255 和每个网卡的定向广播地址发——多网卡机器
// （虚拟机网卡、APIPA 接口）上只发 255.255.255.255 有时出不去。
// 枚举各网卡的定向广播地址（Windows 用 GetAdaptersAddresses）。
// 暴露出来是因为发送端和接收端都可能需要它，而且单测要能覆盖。
std::vector<std::string> local_broadcast_addresses();

// ack_port 是本机接收 NACK 的端口，会随探测包一起广播出去，
// 接收端据此就知道该把 NACK 发到哪。
bool discover_receivers(u16 discovery_port, u16 ack_port, int timeout_ms,
                        std::vector<DiscoveryReply>& out, std::string& error);

// Backward-compatible first-reply helper.
bool discover_receiver(u16 discovery_port, u16 ack_port, int timeout_ms, DiscoveryReply& out,
                       std::string& error);

}  // namespace duo
