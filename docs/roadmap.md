# 路线图

## M0 — 工程骨架（已完成）

- [x] CMake 工程、CI、单测框架
- [x] 无锁 SPSC 环形队列
- [x] 协议打包 / 解析
- [x] XOR 前向纠错
- [x] 令牌桶限速器
- [x] 抖动缓冲（重排 / FEC / NACK / 淘汰）
- [x] UDP 传输层
- [x] DDAPI 采集 + 显示器枚举
- [x] 网络层自检 `duocapd --selftest`
- [x] 采集探测 `duocapd --probe`

## M1 — 单机闭环

- [ ] NVENC HEVC 编码器（`IEncoder` 实现）
- [ ] BGRA → NV12 compute shader 转换
- [ ] D3D11VA 解码器（`IDecoder` 实现）
- [ ] 交换链呈现器（flip model + tearing）
- [ ] `duocapd --stream` / `duocapv --play` 打通本地回环
- [ ] 验收：1440p120 本地 0 丢帧，编码 < 5 ms

## M2 — 双机传输

- [ ] 控制通道（请求 IDR、码率调整、心跳）
- [ ] 抖动缓冲自适应（按到达间隔方差伸缩）
- [ ] NACK 重传（发送端 200 ms 环形缓冲）
- [ ] 验收：高速相机实测 p99 ≤ 35 ms

## M3 — 音频与录制

- [x] WASAPI loopback 采集
- [x] AAC 编码（系统没有 Opus 编码器 MFT，AAC-LC 听感同级）
- [x] 音画时间戳对齐：按累计采样数推算，不随声卡时钟漂移
- [x] 零转码 MKV 录制（Annex-B → 长度前缀 + hvcC/avcC）
- [ ] A 端二次编码推流（RTMP / SRT）—— 需要接 librtmp/libavformat
- [ ] 音频网络传输（当前音频只进录制，未走 UDP）

## M4 — 分析与多路

- [x] 分析 tap（独立队列，忙则丢帧，不阻塞解码）
- [x] 三路独立丢帧策略（呈现丢旧帧 / 分析忙则跳 / 录制一帧不丢）
- [x] 遥测（端到端 p50/p99、丢帧、分析吞吐）
- [ ] 遥测 overlay 渲染到画面上
