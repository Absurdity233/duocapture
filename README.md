# DuoCapture

[![ci](https://github.com/Absurdity233/duocapture/actions/workflows/ci.yml/badge.svg)](https://github.com/Absurdity233/duocapture/actions/workflows/ci.yml)

双机低延迟屏幕采集：**B 机采集并编码 → 局域网 UDP → A 机解码呈现**。

面向「游戏机 + 推流机」这类双机直播与录制场景。全程硬件编解码，不注入进程、不 hook 图形 API。

```
2560×1600 @30fps 实测：编码 2.9 ms · 解码+呈现 0.3 ms · 40 个单测全通过
```

## 特性

**采集与编码（B 端）**

| 能力 | 说明 |
|---|---|
| WGC / DDAPI 双后端 | WGC 用 free-threaded 帧池，不需要 UI 线程消息泵；DDAPI 更轻量、有脏矩形 |
| BGRA → NV12 | 两趟 compute shader，BT.709 limited range，0.3 ms @1440p |
| NVENC 硬编 | H.264 / HEVC / AV1，运行时加载 `nvEncodeAPI64.dll`，**无需 Video Codec SDK** |
| 低延迟参数 | ULTRA_LOW_LATENCY + 禁 B 帧 + 无限 GOP + intra refresh + **VBV 1 帧** |

**传输**

| 能力 | 说明 |
|---|---|
| UDP 自研协议 | 32 字节包头 + 1200 字节载荷，低于 1500 MTU |
| XOR 前向纠错 | 每 16 包 1 个校验包（6.25% 开销），可恢复单包丢失 |
| 令牌桶限速 | 1 ms 粒度摊平，避免整帧突发打满交换机队列 |
| NACK 重传 | 缺包压成**连续区间**回传，发送端保留 200 ms 重传缓冲 |
| 自适应 NACK 延迟 | RFC 3550 抖动估计驱动，2–20 ms 伸缩；抖得越厉害等得越久 |
| 控制通道 | 独立端口：重传请求 / 请求关键帧 |

**接收与呈现（A 端）**

| 能力 | 说明 |
|---|---|
| NVDEC 硬解 | CUVID 直连，**不走 Media Foundation**；NVDEC 不可用时退回 MF（可能是软解） |
| 零延迟呈现 | flip-model 交换链 + tearing + `SetMaximumFrameLatency(1)`，只显示最新帧 |
| 双路独立丢帧语义 | 呈现丢旧帧 / 分析 tap 忙则跳，互不影响 |
| 音频播放 | AAC 解码 → WASAPI，20 ms 缓冲，满了直接丢不阻塞 |

**输出（B 端，编码后直接分流）**

| 能力 | 说明 |
|---|---|
| 零转码录制 | Annex-B → MKV，CPU 开销接近零，含音轨 |
| RTMP / SRT 推流 | ffmpeg 子进程 + 匿名管道，`-c copy` **零转码** |
| 分析 tap（A 端） | 独立线程 + 最新帧覆盖，永不阻塞解码 |
| 插件系统（A 端） | 分析逻辑做成独立 DLL，纯 C ABI，不改主程序即可扩展；自带亮度统计与 TensorRT YOLO 两个示例 |
| 遥测 overlay（A 端） | 端到端 p50/p99、抖动、丢帧、FEC 恢复数、当前 NACK 延迟叠在画面上 |

## 快速开始

需要 **Windows 10 1809+**、**Visual Studio 2022/2026**（含 C++ 工作负载）、**CMake 3.20+**。

```bash
git clone --recurse-submodules https://github.com/Absurdity233/duocapture.git
cd duocapture
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

已经 clone 过的话补一下子模块（`plugins/yolo_det`）：

```bash
git submodule update --init --recursive
```

不需要那个插件的话 `--depth 1` 或直接跳过——它不在主构建里，
CMake 完全不会碰它。

或者用带 MSVC 环境配置的脚本：

```bat
scripts\build.cmd
```

**单机自检**（不需要第二台机器）：

```bash
build\duocapd.exe --list-displays      # 确认能枚举到显示器
build\duocapd.exe --selftest           # 网络层自检：打包 → FEC → 乱序 → 重组
build\duocapd.exe --probe 5            # 采集探测：抓 5 秒，打印帧率
```

**抖动缓冲基准**（量化 FEC 恢复开销）：

```bat
scripts\bench.cmd
```

## 双机使用

### 1. 网络

一根网线直连，两台机器配同网段静态 IP：

```
B（游戏机）  192.168.10.1 / 255.255.255.0
A（推流机）  192.168.10.2 / 255.255.255.0
```

> 端口默认 **50000**（视频）/ **50001**（控制）。不要用 5000——Windows 上常被系统服务占用。

### 2. B 端推流

```bash
build\duocapd.exe --stream --target 192.168.10.2 --fps 120 --bitrate 80
```

### 3. A 端接收

```bash
build\duocapv.exe --play --peer 192.168.10.1 --overlay
```

`--peer` 让 A 端能把 NACK 和关键帧请求回传给 B，弱网下必须加。

### 完整示例

```bash
# B：推流 + 本地录 MKV + 带系统声音
build\duocapd.exe --stream --target 192.168.10.2 --fps 120 --bitrate 80 ^
    --audio --record capture.mkv

# A：呈现 + 叠加统计 + 下游分析
build\duocapv.exe --play --peer 192.168.10.1 --overlay --analyze

# B：同时推 RTMP（需要系统 PATH 里有 ffmpeg）
build\duocapd.exe --stream --target 192.168.10.2 --audio ^
    --push "rtmp://live.example.com/app/STREAM_KEY"
```

> 录制和推流都在 **B 端**做：码流刚编码完就分叉，不经过网络，也就不会因丢包丢帧。
> 这也是「游戏机只编码一次、推流机只管分发」这个拓扑的实际边界——A 端目前只做呈现与分析。

## 命令行参考

### `duocapd`（B 端 / 采集端）

| 参数 | 说明 |
|---|---|
| `--list-displays` | 列出可采集的显示器 |
| `--selftest` | 网络层自检 |
| `--probe [秒数]` | 采集探测，默认 5 秒 |
| `--stream` | 启动推流 |
| `--target <IP>` | 目标主机（必填，除非用 `--playback`） |
| `--port <端口>` | 目标端口，默认 50000 |
| `--display <序号>` | 显示器序号，默认 0 |
| `--fps <帧率>` | 默认 120 |
| `--bitrate <Mbps>` | 默认 80 |
| `--codec <hevc\|h264\|av1>` | 默认 hevc |
| `--no-fec` | 关闭前向纠错（有线上可关，省延迟） |
| `--no-intra-refresh` | 改用周期性 IDR |
| `--audio` | 采集系统声音 |
| `--audio-bitrate <kbps>` | 音频码率，默认 128 |
| `--record <文件.mkv>` | 零转码录制 |
| `--push <URL>` | RTMP / SRT 推流地址 |
| `--ffmpeg <路径>` | ffmpeg 路径，默认从 PATH 找 |
| `--playback <文件>` | 从裸流回放（调试用，不采集不编码） |
| `--ddapi` | 用 DXGI Desktop Duplication 而非 WGC |

### `duocapv`（A 端 / 接收端）

| 参数 | 说明 |
|---|---|
| `--selftest` | 网络层自检 |
| `--monitor [端口] [秒数]` | 只收包统计，不解码 |
| `--play` | 接收并呈现 |
| `--port <端口>` | 监听端口，默认 50000 |
| `--width` / `--height` | 窗口尺寸，默认跟随码流 |
| `--no-window` | 无窗口，只解码 |
| `--peer <IP>` | 回传 NACK / 关键帧请求到该地址 |
| `--peer-control-port <端口>` | 对端控制端口，默认 50001 |
| `--overlay` | 画面上叠加延迟/丢帧统计 |
| `--analyze` | 启用分析 tap |
| `--plugin-dir <目录>` | 加载该目录下的插件 DLL |
| `--plugin <名称>` | 指定插件（默认用第一个分析插件） |
| `--plugin-config <串>` | 传给插件的配置字符串 |

## 架构

```
B（采集端）                                        A（接收端）
┌──────────────────────────────┐                  ┌────────────────────────────────┐
│ WGC / DDAPI 采集              │                  │ UDP :50000                     │
│   ↓ D3D11 纹理 (BGRA)         │                  │   ├─ 音频包 → AAC 解码 → WASAPI │
│ compute shader → NV12         │                  │   └─ 视频包 → 抖动缓冲           │
│   ↓                           │   UDP :50000     │              ├─ FEC 恢复        │
│ NVENC (H.264/HEVC/AV1)        │ ───────────────→ │              ├─ 重排            │
│   ↓ Annex-B 码流              │                  │              └─ 窗口淘汰        │
│ 切片 + FEC + 令牌桶限速        │                  │                    ↓           │
│                               │   UDP :50001     │              NVDEC 解码         │
│ WASAPI 环回 → AAC             │ ←─────────────── │                    ↓           │
│                               │  NACK / 请求IDR   │         ┌──────────┴──────────┐ │
│ 控制通道（监听 :50001）        │                  │         ↓                     ↓ │
│                               │                  │      呈现+overlay          分析tap│
│ 可选：MKV 录制 / ffmpeg 推流   │                  │      (丢旧帧)           (忙则跳) │
└──────────────────────────────┘                  └────────────────────────────────┘
```

**两路消费者的丢帧语义完全不同，这是设计的核心约束：**

| 消费者 | 位置 | 语义 | 原因 |
|---|---|---|---|
| 呈现 | A 端 | 只显示最新帧，旧的直接丢 | 延迟优先 |
| 分析 tap | A 端 | 忙就跳过这一帧 | 吞吐优先，绝不阻塞解码 |
| 录制 / 推流 | **B 端** | 一帧都不能丢 | 完整性优先；在编码后分叉，不经过网络 |

A 端的呈现和分析用两条独立队列，不共用。B 端的录制/推流直接消费编码器输出，与网络发送互不阻塞。

详细设计见 [`docs/architecture.md`](docs/architecture.md)，线格式见 [`docs/protocol.md`](docs/protocol.md)，延迟预算见 [`docs/latency.md`](docs/latency.md)。

## 延迟预算

1440p120 估算（**部分已实测，glass-to-glass 尚未在真实双机上验证**）：

| 环节 | 耗时 | 状态 |
|---|---|---|
| 采集（等 vblank） | 0–8.3 ms | 取决于刷新率 |
| BGRA → NV12 | 0.3 ms | ✅ 实测 |
| NVENC 编码 | 2.9 ms | ✅ 实测（2560×1600@30） |
| 打包 + 限速 | 0.2–1 ms | — |
| 网络（千兆直连） | 0.2–0.5 ms | — |
| 抖动缓冲 | 2–12 ms | 自适应 |
| NVDEC 解码 | 1–2 ms | — |
| 呈现（等 vblank） | 0–8.3 ms | tearing 时可到 0 |
| **合计** | **约 15–30 ms** | 中位目标 ≤ 25 ms |

**刷新率是最大的杠杆。** 采集和呈现各等一个 vblank，60 Hz 屏幕光这两段就吃掉 33 ms。两端都上 120 Hz 才能压到 20 ms 出头。

glass-to-glass 必须用 240 fps 相机拍两块屏幕上的毫秒计时器来测——程序内埋点只能测到「采集→呈现」，测不到显示器响应和扫描输出。

## 已知限制

| 限制 | 说明 |
|---|---|
| **glass-to-glass 未实测** | 单机回环验证过，真双机 p99 待测（roadmap 最后一项） |
| **RTMP 未端到端验证** | 管道链路和 FLV/MKV 封装已验证，RTMP 握手需要真实服务端 |
| RTMP 不支持 HEVC | Twitch / YouTube / B站 只收 H.264，用 HEVC 推会被拒（代码会告警） |
| D3D11 零拷贝未打通 | NV12 的 UV 平面拿不到第二个 CUDA array slice，目前走 CPU 拷贝（0.3 ms，够用） |
| 静态桌面 WGC 不推帧 | 画面不变时不产生新帧，属正常行为；测试请用 `--playback` |
| 音频不重传 | 音频包丢了就丢了，不参与 NACK（补发只会增加延迟） |
| A 端不录制不推流 | 录制/推流只在 B 端；A 端只做呈现、分析与插件 |
| 无输入注入 | 只看不控 |

## 开发

```bash
build\duo_tests.exe        # 40 个单元测试
```

测试覆盖：环形队列、协议打包、FEC 恢复（含尾组与乱序不误恢复）、限速器、抖动缓冲（重排 / 自适应 NACK 延迟 / 重传后恢复）、SPS 解析、分析 tap 丢帧语义、NACK 区间压缩、控制通道序列化、插件 ABI 校验。

```
include/duocapture/     公共接口
  capture/              采集源、色彩转换
  encode/               编码器接口
  decode/               解码器接口、SPS 解析
  net/                  协议、FEC、限速、抖动缓冲、传输、控制通道
  present/              呈现器、遥测 overlay
  plugin/               插件 ABI 与宿主
  record/               MKV 封装、推流
  audio/                WASAPI 采集/播放、AAC 编解码
  pipeline/             Sender / Receiver / 分析 tap
src/                    实现
apps/duocapd            B 端程序
apps/duocapv            A 端程序
tests/                  单元测试
plugins/luma_stats/     示例插件（平均亮度 + 黑帧检测）
plugins/yolo_det/       示例插件（TensorRT YOLO 目标检测，Git submodule）
scripts/                构建、测试、基准脚本
docs/                   设计文档（含 plugins.md 插件开发指南）
third_party/            nv-codec-headers（仅头文件，用于 NVENC/CUVID 声明）
```

约 13,000 行 C++20（含测试）。

## 合规

本项目只做**屏幕采集与显示**：不注入进程、不 hook 图形 API、不读取游戏内存、不规避任何技术保护措施。

采集走系统公开 API（WGC / DDAPI），遇到 DRM / WDA 保护内容会如实返回黑帧并停止，不尝试绕过。

使用前请阅读 [`docs/compliance.md`](docs/compliance.md)。采集他人屏幕需取得同意；录制内容若含他人个人信息，需按当地隐私法规处理。

## 许可

MIT，见 [LICENSE](LICENSE)。
