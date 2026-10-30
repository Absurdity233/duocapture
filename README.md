# DuoCapture

双机低延迟屏幕采集：**B 机采集屏幕并编码 → 局域网 UDP 传输 → A 机解码呈现 / 录制 / 推流**。

面向「游戏机 + 推流机」这类双机直播与录制场景，目标是 1440p120 下 glass-to-glass 中位延迟 20–25 ms。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 工程骨架、协议、传输、FEC、抖动缓冲、采集探测 | ✅ 已完成 |
| M1 | WGC 采集 + NVENC 编码 + 本地解码回显 | 🚧 进行中 |
| M2 | 双机 UDP 传输 + 控制通道 | ⏳ 待开始 |
| M3 | 音频 + remux 录制 + 推流 | ⏳ 待开始 |
| M4 | 分析 tap + 双路分发 | ⏳ 待开始 |

**已实现（可直接运行、有单测覆盖）**

- 无锁 SPSC 环形队列
- 协议打包 / 解包（32 字节包头，1200 字节载荷）
- XOR 前向纠错（每 N 包 1 个校验包，可恢复 1 个丢包）
- 令牌桶限速器（按 1 ms 粒度摊平发送，避免突发打满交换机队列）
- 抖动缓冲（乱序重排、FEC 恢复、NACK 缺失检测、窗口淘汰）
- UDP 传输层（Windows / POSIX）
- DXGI Desktop Duplication 采集与显示器枚举（Windows）

**已定义接口、尚未实现**

- NVENC 编码器（需要 NVIDIA Video Codec SDK，`-DDUO_ENABLE_NVENC=ON`）
- NVDEC / D3D11VA 解码器
- 交换链呈现器
- 完整 Sender / Receiver 管线

## 快速开始

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

```bash
# 列出可采集的显示器
duocapd --list-displays

# 跑一遍网络层自检（打包 → FEC → 乱序 → 重组）
duocapd --selftest

# 采集探测：抓 5 秒，打印帧率与帧尺寸
duocapd --probe 5

# 监听 UDP，打印收包统计
duocapv --monitor 5000
```

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
