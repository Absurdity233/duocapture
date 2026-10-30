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

- [ ] WASAPI loopback 采集 + Opus 编码
- [ ] 音画时间戳对齐（目标偏差 < 20 ms）
- [ ] 原始码流 remux 录制（MP4 / MKV，零转码）
- [ ] A 端二次编码推流（RTMP / SRT）

## M4 — 分析与多路

- [ ] 分析 tap（零拷贝读 NV12 纹理）
- [ ] 三路独立丢帧策略（呈现 / 录制 / 分析）
- [ ] 遥测 overlay（延迟、丢包、FEC 恢复率）
