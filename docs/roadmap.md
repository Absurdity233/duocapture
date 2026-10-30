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

- [x] NVENC HEVC 编码器（`IEncoder` 实现）
- [x] BGRA → NV12 compute shader 转换
- [x] D3D11VA 解码器（`IDecoder` 实现）
- [x] 交换链呈现器（flip model + tearing）
- [x] `duocapd --stream` / `duocapv --play` 打通本地回环
- [x] 验收：2560x1600 本地 0 丢帧，编码 2.9 ms

## M2 — 双机传输

- [x] 控制通道（NACK / 请求关键帧）
- [x] 抖动缓冲自适应（RFC 3550 抖动估计 → NACK 延迟 2-20ms 伸缩）
- [x] NACK 重传（发送端 200ms 重传缓冲 + 区间压缩）
- [ ] 验收：高速相机实测 p99 ≤ 35 ms

## M3 — 音频与录制

- [x] WASAPI loopback 采集
- [x] AAC 编码（系统没有 Opus 编码器 MFT，AAC-LC 听感同级）
- [x] 音画时间戳对齐：按累计采样数推算，不随声卡时钟漂移
- [x] 零转码 MKV 录制（Annex-B → 长度前缀 + hvcC/avcC）
- [x] 音频网络传输（UDP，`kFlagAudio` 标志位，向后兼容）
- [x] RTMP / SRT 推流（ffmpeg 子进程 + 匿名管道，`-c copy` 零转码）

## M4 — 分析与多路

- [x] 分析 tap（独立队列，忙则丢帧，不阻塞解码）
- [x] 三路独立丢帧策略（呈现丢旧帧 / 分析忙则跳 / 录制一帧不丢）
- [x] 遥测（端到端 p50/p99、丢帧、分析吞吐）
- [x] 遥测 overlay 渲染到画面上（GDI 画字 → D3D11 纹理 → alpha 混合）
