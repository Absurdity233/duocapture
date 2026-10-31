// 发现协议的编解码与地址筛选。
//
// 这里只验证「线上的字节」和「地址挑得对不对」，不跑真实收发——
// 收发需要两个进程/两台机器，属于集成测试的范畴。

#include <string>
#include <vector>

#include "duocapture/net/discovery.h"
#include "testfw.h"

using namespace duo;

DUO_TEST(discovery_probe_roundtrip) {
  DiscoveryProbe probe{};
  probe.nonce = 0xDEADBEEFu;
  probe.ack_port = 50011;

  const Bytes wire = serialize_discovery_probe(probe);
  DUO_CHECK_EQ(wire.size(), kDiscoveryProbeSize);

  DiscoveryProbe parsed{};
  DUO_CHECK(parse_discovery_probe(ByteSpan(wire.data(), wire.size()), parsed));
  DUO_CHECK_EQ(parsed.nonce, 0xDEADBEEFu);
  DUO_CHECK_EQ(parsed.ack_port, 50011);
}

DUO_TEST(discovery_probe_rejects_garbage) {
  DiscoveryProbe probe{};
  probe.nonce = 0x11223344u;
  probe.ack_port = 50001;
  const Bytes wire = serialize_discovery_probe(probe);

  DiscoveryProbe parsed{};

  // magic 不对
  Bytes bad = wire;
  bad[0] = 'X';
  DUO_CHECK(!parse_discovery_probe(ByteSpan(bad.data(), bad.size()), parsed));

  // 长度不符
  DUO_CHECK(!parse_discovery_probe(ByteSpan(wire.data(), wire.size() - 1), parsed));
  DUO_CHECK(!parse_discovery_probe(ByteSpan(wire.data(), wire.size() + 1), parsed));

  // 版本不符
  bad = wire;
  bad[4] = static_cast<duo::u8>(kDiscoveryVersion + 1);
  DUO_CHECK(!parse_discovery_probe(ByteSpan(bad.data(), bad.size()), parsed));

  // 空数据
  DUO_CHECK(!parse_discovery_probe(duo::ByteSpan(), parsed));
}

DUO_TEST(discovery_reply_roundtrip) {
  DiscoveryReply reply{};
  reply.nonce = 0x12345678u;
  reply.video_port = 6000;

  const Bytes wire = serialize_discovery_reply(reply);
  DUO_CHECK_EQ(wire.size(), kDiscoveryReplySize);

  DiscoveryReply parsed{};
  DUO_CHECK(parse_discovery_reply(ByteSpan(wire.data(), wire.size()), parsed));
  DUO_CHECK_EQ(parsed.nonce, 0x12345678u);
  DUO_CHECK_EQ(parsed.video_port, 6000);
}

DUO_TEST(discovery_probe_is_not_a_reply) {
  DiscoveryProbe probe{};
  probe.nonce = 1;
  const Bytes wire = serialize_discovery_probe(probe);

  DiscoveryReply parsed{};
  // 两者长度相同，只能靠 magic 区分——这正是 magic 存在的意义。
  DUO_CHECK(!parse_discovery_reply(ByteSpan(wire.data(), wire.size()), parsed));
}

DUO_TEST(discovery_broadcast_addresses_are_usable) {
  const std::vector<std::string> addrs = local_broadcast_addresses();
  for (const std::string& a : addrs) {
    // 回环和链路本地地址发出去没人收，留着只会拖慢发现。
    DUO_CHECK(a.rfind("127.", 0) != 0);
    DUO_CHECK(a.rfind("169.254.", 0) != 0);
  }
}
