# DuoCapture

双机低延迟屏幕采集：**B 机采集屏幕并编码 → 局域网 UDP 传输 → A 机解码呈现 / 录制 / 推流**。

面向「游戏机 + 推流机」这类双机直播与录制场景，目标是 1440p120 下 glass-to-glass 中位延迟 20–25 ms。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 工程骨架、协议、传输、FEC、抖动缓冲、采集探测 | ✅ 已完成 |
| M1 | WGC/DDAPI 采集 + NV12 转换 + NVENC 编码 + 解码 + 呈现 | ✅ 已完成 |
| M2 | 双机 UDP 传输 + FEC + 限速 + 抖动缓冲 + 编解码协商 | ✅ 已完成 |
| M3 | 音频（采集/传输/播放）+ 零转码 MKV 录制 + RTMP/SRT 推流 | ✅ 已完成 |
| M4 | 分析 tap + 遥测（p50/p99）+ 画面 overlay | ✅ 已完成 |

**已实现（可直接运行、有单测覆盖）**

- 无锁 SPSC 环形队列
- 协议打包 / 解包（32 字节包头，1200 字节载荷）
- XOR 前向纠错（每 N 包 1 个校验包，可恢复 1 个丢包）
- 令牌桶限速器（按 1 ms 粒度摊平发送，避免突发打满交换机队列）
- 抖动缓冲（乱序重排、FEC 恢复、NACK 缺失检测、窗口淘汰）
- UDP 传输层（Windows / POSIX）
- DXGI Desktop Duplication 采集与显示器枚举（Windows）

- NVENC 硬件编码器（运行时动态加载 `nvEncodeAPI64.dll`，无需 SDK 头文件即可构建）
- BGRA → NV12 GPU 转换（两趟 compute shader，BT.709 limited range）
- NVDEC 硬件解码（CUVID 直连，不依赖 MF）；不可用时回退 Media Foundation
- flip-model 交换链呈现（DXGI tearing，最大帧延迟 1）
- 完整 Sender / Receiver 多线程管线
- 编码格式随包头发送，接收端自动选择解码器
- SPS 解析（H.264 / HEVC）以确定解码器输入的分辨率
- WASAPI 环回音频采集 + AAC 编码，音画按采样数对齐（不随声卡时钟漂移）
- 零转码 MKV 录制：Annex-B → 长度前缀 + hvcC/avcC，边录边写
- 分析 tap：与呈现完全独立，「忙则丢帧」，绝不阻塞解码
- 遥测：端到端延迟 p50 / p99、丢帧、分析吞吐
- 音频走 UDP（`kFlagAudio`），A 端解码播放，音画按采样数对齐
- RTMP / SRT 推流：ffmpeg 子进程 + 匿名管道，`-c copy` 零转码
- 控制通道：NACK 重传（区间压缩）+ 请求关键帧
- 抖动缓冲自适应：RFC 3550 抖动估计驱动 NACK 延迟（2–20ms）
- 遥测 overlay：延迟/丢包/FEC 统计直接叠在画面上

## 快速开始

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

```bash
# 列出可采集的显示器
duocapd --list-displays

# 网络层自检（打包 → FEC → 乱序 → 重组）
duocapd --selftest

# 采集探测：抓 5 秒，打印帧率与帧尺寸
duocapd --probe 5

# B 端推流
duocapd --stream --target 192.168.1.20 --fps 120 --bitrate 80

# A 端接收并呈现
duocapv --play

# 无持续画面来源时，用录制的裸流回放来验证传输/解码/呈现
duocapd --stream --playback capture.h264 --target 127.0.0.1

# 录制（零转码 MKV，含系统声音）
duocapd --stream --target 127.0.0.1 --record capture.mkv --audio

# A 端启用分析 tap（下游 CV/OCR 消费者）与画面统计
duocapv --play --analyze --overlay

# A 端回传 NACK / 请求关键帧（填发送端地址）
duocapv --play --peer 192.168.10.1

# 推流（需要系统里有 ffmpeg）
duocapd --stream --target 127.0.0.1 --audio --push "rtmp://live.example.com/app/key"
duocapd --stream --target 127.0.0.1 --audio --push "srt://1.2.3.4:9000?mode=caller"
```

端口默认 50000（视频）/ 50001（控制）。避开 5000，Windows 上该端口常被系统服务占用。

## 架构

```
B（采集端）                                  A（接收端）
WGC/DDAPI 采集 ──→ NV12 ──→ NVENC HEVC ──UDP──→ 抖动缓冲 ─┬─→ 原始码流 remux 录制
WASAPI loopback ──→ Opus ────────────────────→ 音频缓冲 ──┘
                                          └─→ NVDEC ──┬─→ 呈现
                                                      ├─→ 二次编码推流
                                                      └─→ 分析 tap
```

详细设计见 [`docs/architecture.md`](docs/architecture.md)，线格式见 [`docs/protocol.md`](docs/protocol.md)，延迟预算见 [`docs/latency.md`](docs/latency.md)。

## 合规

本项目只做**屏幕采集与显示**，不注入进程、不 hook 图形 API、不读取游戏内存、不规避任何技术保护措施。

使用前请阅读 [`docs/compliance.md`](docs/compliance.md)。在受保护内容（DRM / WDA）上采集会得到黑帧，本项目的设计是**如实上报并停止**，而非绕过。

## 目录结构

```
include/duocapture/   公共头文件（接口定义）
src/                  实现
apps/duocapd          B 端采集/发送程序
apps/duocapv          A 端接收/呈现程序
tests/                单元测试
docs/                 设计文档
```

## 许可

MIT，见 [LICENSE](LICENSE)。
