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
  uint16_t reserved;       // 数据包：高 8 位 codec；校验包：尾包真实载荷长度
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
- 校验包：只对同组所有数据包的**载荷**（补齐到 1200 字节）做 XOR

校验只覆盖载荷，包头由接收端从同组兄弟包重建。若该校验组包含帧尾包，
其真实载荷长度放在校验包的 `reserved` 字段里（整个 16 位，不用 8 位——
尾包常常超过 255 字节）。

恢复出的数据包 `pkt_index = 组基址 + 组内缺号`，组基址由
`pkt_index - fec_index` 推出；帧尾组通常不满 `fec_count`，不能假设
它是 `fec_group * group_size`。

## 端口

| 用途 | 默认端口 | 协议 |
|---|---|---|
| 视频 | 5000/UDP | 本格式 |
| 控制 | 5001/UDP | 请求 IDR、码率调整、心跳 |

## 兼容性

- 版本号不匹配时接收端直接丢弃，不做前向兼容
- 未知 flags 位忽略
- `pkt_count` 为 0 或 `payload_len` 超过 1200 的包视为非法
