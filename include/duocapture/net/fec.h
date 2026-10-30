#pragma once

#include <cstddef>
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
// 恢复接口设计成「原地」的：直接操作抖动缓冲里的数据包槽位，不做
// 整帧拷贝。旧实现每收到一个包就重建一次工作集（深拷贝整帧），
// 一帧 40 包就是 40 次全帧拷贝，120fps 下约 230 MB/s 的纯浪费。
class FecCodec {
 public:
  // 为一帧填充 FEC 字段（修改 packets 的 fec_group / fec_index / fec_count），
  // 并返回该帧的校验包。
  [[nodiscard]] static std::vector<Packet> encode_frame(std::vector<Packet>& packets,
                                                        std::size_t group_size);

  // 单组恢复所需的最小视图。调用方按组把「已收到的包」和「校验包」
  // 的指针填进来，恢复出的包写回 out_recovered。
  //
  // data: 该组已收到的数据包（顺序无关）
  // parity: 该组的校验包（通常只有 1 个）
  // group_index: FEC 组号（只用于写回包头）
  // group_base: 该组 0 号槽位对应的帧内 pkt_index。注意不能由
  //   group_index * group_size 推出来：帧尾那组通常不满，它的
  //   fec_count 是实际跨度而不是 group_size。
  // group_size: 该组应有的数据包总数
  // out_missing_index: 恢复出的包在组内的序号
  // out_recovered: 恢复出的完整包
  //
  // 只有当组内恰好缺 1 个包时才可能成功。
  static bool recover_group(const std::vector<const Packet*>& data, const Packet* parity,
                            u8 group_index, u16 group_base, std::size_t group_size,
                            u16& out_missing_index, Packet& out_recovered);

  // 便捷版本：从一个包列表里尝试恢复任意一组。保留给测试和小规模场景。
  // 高频路径应当用 recover_group。
  static bool recover_one(std::vector<Packet>& packets);
};

}  // namespace duo
