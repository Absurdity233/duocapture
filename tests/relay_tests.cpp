#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <vector>

#include "duocapture/net/packet.h"
#include "duocapture/net/relay.h"
#include "duocapture/net/transport.h"
#include "testfw.h"

using namespace duo;

DUO_TEST(relay_forwards_valid_video_bytes_unchanged_to_each_client) {
  UdpSocket upstream, client_a, client_b;
  DUO_CHECK(ok(upstream.open("127.0.0.1", 0)));
  DUO_CHECK(ok(client_a.open("127.0.0.1", 0)));
  DUO_CHECK(ok(client_b.open("127.0.0.1", 0)));
  RelayConfig config;
  config.listen_video_port = 45100;
  config.listen_control_port = 45101;
  config.sender_control_port = 45102;
  config.clients = {{"127.0.0.1", client_a.local_port()}, {"127.0.0.1", client_b.local_port()}};
  RelayServer relay;
  DUO_CHECK(ok(relay.start(config)));

  PacketHeader header{};
  header.magic = kProtocolMagic;
  header.version = kProtocolVersion;
  header.frame_id = 42;
  header.pkt_index = 3;
  header.pkt_count = 7;
  header.capture_ts_ns = 123456789;
  const std::array<u8, 4> payload{1, 2, 3, 4};
  const Bytes wire = serialize(header, ByteSpan(payload.data(), payload.size()));
  DUO_CHECK(ok(upstream.send_to(ByteSpan(wire.data(), wire.size()), "127.0.0.1", config.listen_video_port)));

  for (UdpSocket* client : {&client_a, &client_b}) {
    std::array<u8, 128> buffer{};
    std::size_t received = 0;
    std::string host;
    u16 port = 0;
    DUO_CHECK(ok(client->recv_from(MutByteSpan(buffer.data(), buffer.size()), received, host, port, 1000)));
    DUO_CHECK_EQ(received, wire.size());
    DUO_CHECK(std::equal(wire.begin(), wire.end(), buffer.begin()));
  }
  relay.stop();
  const RelayStats stats = relay.stats();
  DUO_CHECK_EQ(stats.video_datagrams_received, 1u);
  DUO_CHECK_EQ(stats.video_datagrams_forwarded, 2u);
}
