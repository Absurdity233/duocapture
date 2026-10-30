# 线格式

## 包头（32 字节，小端）

```c
struct PacketHeader {
  uint32_t magic;          // 0x314F5544 = 'D''U''O''1'
  uint16_t version;        // 1
  uint8_t  flags;          // 见下表
  uint8_t  fec_group;      // 帧内 FEC 组序号
  uint8_t  fec_index;      // 组内序号；== fec_count 表示校验包
  uint8_t  fec_count;      // 该组数据包数量（0 表示未启用 FEC）
  uint16_t reserved;       // 0
  uint32_t frame_id;       // 帧序号，单调递增
  uint16_t pkt_index;      // 帧内包序号；校验包为 0xF000 + 组号
  uint16_t pkt_count;      // 该帧数据包总数
  uint64_t capture_ts_ns;  // 采集时刻
  uint32_t payload_len;    // 载荷字节数
};
```

## flags

| 位 | 名称 | 含义 |
|---|---|---|
| 0x01 | KEY | 关键帧 |
| 0x02 | FRAME_START | 帧的第一个包 |
| 0x04 | FRAME_END | 帧的最后一个包 |
| 0x08 | FEC_PARITY | 该包是 FEC 校验包 |

## 载荷

- 数据包：编码器输出的字节切片，顺序拼接即为该帧的完整码流
- 校验包：对同组所有数据包的**完整字节**（包头 + 载荷，补齐到 1232 字节）做 XOR

校验覆盖包头，因此恢复出的包自带正确的 `pkt_index` / `payload_len`，不需要额外索引。

## 端口

| 用途 | 默认端口 | 协议 |
|---|---|---|
| 视频 | 5000/UDP | 本格式 |
| 控制 | 5001/UDP | 请求 IDR、码率调整、心跳 |

## 兼容性

- 版本号不匹配时接收端直接丢弃，不做前向兼容
- 未知 flags 位忽略
- `pkt_count` 为 0 或 `payload_len` 超过 1200 的包视为非法
