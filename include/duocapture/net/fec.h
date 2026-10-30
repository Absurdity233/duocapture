#pragma once

#include <vector>

#include "duocapture/net/packet.h"

namespace duo {

// 帧内 XOR 前向纠错。
//
// 把一帧的数据包按 group_size 分组，每组产生 1 个校验包。校验包对
// 组内各包的**载荷**（补齐到固定长度）做异或。恢复时载荷由异或得出，
// 包头从同组兄弟包重建——两者合并即可还原出与原始包逐字节一致的结果。
// 组内包含帧尾包时，其真实载荷长度存放在校验包的 reserved 字段里。
//
// 代价是每 group_size 个包多 1 个包的开销（group_size=16 时约 6.25%）。
// 局域网上丢包罕见，但丢一个包就是一次可见花屏，这个开销值得。
class FecCodec {
 public:
  // 为一帧填充 FEC 字段（修改 packets 的 fec_group / fec_index / fec_count），
  // 并返回该帧的校验包。
  [[nodiscard]] static std::vector<Packet> encode_frame(std::vector<Packet>& packets,
                                                        std::size_t group_size);

  // 尝试恢复一个缺失的数据包。成功则把恢复出的包追加到 packets 并返回 true。
  // 一次调用最多恢复一个包；可循环调用直到返回 false。
  static bool recover_one(std::vector<Packet>& packets);
};

}  // namespace duo
