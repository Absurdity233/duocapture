# 配置文件

两个程序都支持从 JSON 文件读取配置：

```bat
duocapd --config sender.json --stream
duocapv --config receiver.json --play --peer 192.168.10.1
```

## 优先级

```
结构体默认值  <  配置文件  <  命令行参数
```

命令行**逐项覆盖**文件：文件里写 `"fps": 60`，命令行再给 `--fps 120`，最终是 120。
临时调参不该逼人去改文件，所以命令行永远赢。

文件里没写的键保持默认值——配置文件不需要写全。

`--config` 可以写在模式前面（`duocapd --config x.json --stream`），也可以写在后面。

## 错误处理

配置有问题时**不会带着半个配置跑起来**，而是把所有能发现的问题一次列出来：

```
读取配置 sender.json 失败:
  sender.json:3:10: "fps" 应为整数，实际是字符串
  sender.json:5:12: "codec" 只能是 "h264" / "hevc" / "av1"，实际是 "vp9"
  sender.json:6:43: "fec.group_size" 超出允许范围 [2, 64]，实际 200
  sender.json: 未知配置项 "bitrate_mbp"
```

三条原则：

1. **带行号和列号**。配置文件是手写的，只说「fps 错了」等于没说。
2. **一次报全部**。改一个键就跑一次，二十个键要跑二十次。
3. **拼错的键必须报错**。`"bitrate_mbp"` 如果被静默忽略，用户会以为是 bug
   （"配了没生效"），而不是自己拼错了。未知键是硬错误。

类型或范围错误的那一项保持默认值，不会写入半个值。

## 严格 JSON

解析器只接受标准 JSON，额外支持 UTF-8 BOM（Windows 记事本会加）。

明确拒绝：

| 写法 | 原因 |
|---|---|
| `{ "a": 1, }` | 尾逗号 |
| `{ "a": 1, "a": 2 }` | 重复键——几乎总是复制粘贴留下的，静默取最后一个会让人查半天 |
| `// 注释` | 配置文件不是编程语言 |
| `1.` / `.5` / `01` / `1e` | 数字语法错误 |
| `NaN` / `Infinity` | 不是合法 JSON |
| 嵌套超过 32 层 | 防止恶意输入打穿递归栈 |

## 发送端（duocapd）

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `target_host` | 字符串 | `127.0.0.1` | 目标主机 |
| `target_port` | 整数 1–65535 | 50000 | 目标端口 |
| `control_port` | 整数 1–65535 | 50001 | 控制通道端口（收 NACK） |
| `display_index` | 整数 0–15 | 0 | 显示器序号 |
| `prefer_wgc` | 布尔 | `true` | `false` = 用 DDAPI |
| `fps` | 整数 1–240 | 120 | 采集/编码帧率 |
| `bitrate_mbps` | 数字 0.1–2000 | 80 | 编码码率 |
| `codec` | 字符串 | `hevc` | `h264` / `hevc` / `av1` |
| `fec.enabled` | 布尔 | `true` | 前向纠错开关 |
| `fec.group_size` | 整数 2–64 | 16 | 每 N 个数据包 1 个校验包 |
| `intra_refresh` | 布尔 | `true` | `false` = 改用周期性 IDR |
| `gop_frames` | 整数 0–1000000 | 0 | IDR 间隔，0 = 无限 |
| `pacer_mbps` | 数字 0–10000 | 0 | 限速器速率，0 = 按码率自动留 2× 余量 |
| `retransmit_window_ms` | 整数 0–5000 | 200 | 重传缓冲时长 |
| `record_path` | 字符串 | 空 | 零转码录 MKV，空 = 不录 |
| `push_url` | 字符串 | 空 | RTMP / SRT 地址，空 = 不推 |
| `ffmpeg_path` | 字符串 | 空 | 空 = 从 PATH 找 |
| `audio.enabled` | 布尔 | `false` | 采集系统声音 |
| `audio.bitrate_kbps` | 数字 8–512 | 128 | 音频码率 |
| `playback.path` | 字符串 | 空 | 裸流回放，空 = 正常采集 |
| `playback.fps` | 整数 1–240 | 20 | 回放帧率 |

示例：

```json
{
  "target_host": "192.168.10.2",
  "fps": 120,
  "bitrate_mbps": 80,
  "codec": "hevc",
  "fec": { "enabled": true, "group_size": 16 },
  "audio": { "enabled": true, "bitrate_kbps": 128 },
  "record_path": "capture.mkv"
}
```

## 接收端（duocapv）

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `listen_port` | 整数 1–65535 | 50000 | 视频监听端口 |
| `width` / `height` | 整数 0–8192 | 0 | 窗口尺寸，0 = 跟随码流 |
| `jitter_window` | 整数 1–256 | 8 | 抖动缓冲窗口（帧） |
| `nack_delay_ms` | 整数 0–200 | 3 | NACK 延迟；之后由抖动自适应调整 |
| `nack_delay_adaptive` | 布尔 | `true` | `false` = 钉住上面的值 |
| `show_window` | 布尔 | `true` | `false` = 只解码不显示 |
| `vsync` | 布尔 | `false` | `false` = tearing，延迟优先 |
| `analyze` | 布尔 | `false` | 启用分析 tap |
| `play_audio` | 布尔 | `true` | A 端出声 |
| `overlay` | 布尔 | `false` | 叠加延迟/丢帧统计 |
| `boxes_overlay` | 布尔 | `true` | 绘制插件上报的检测框 |
| `peer_host` | 字符串 | 空 | 回传 NACK / 请求关键帧的目标，空 = 不回传 |
| `peer_control_port` | 整数 1–65535 | 50001 | 对端控制端口 |

示例：

```json
{
  "listen_port": 50000,
  "jitter_window": 12,
  "vsync": false,
  "overlay": true,
  "peer_host": "192.168.10.1",
  "boxes_overlay": true
}
```

## 调参提示

**弱网**（WiFi、交换机拥塞）：

- 接收端 `jitter_window` 8 → 16~32：给乱序包更多时间自己到齐，少发无效 NACK
- 保持 `nack_delay_adaptive: true`：抖动大时自动等更久
- 发送端 `fec.group_size` 保留 16；降到 8 能救更多单点丢包，但校验开销翻倍

**有线上**（低延迟优先）：

- 发送端 `fec.enabled: false`：省掉 6.25% 开销和一次校验包发送
- 接收端 `jitter_window` 降到 4：窗口小一点，组装更早
- 接收端 `vsync: false`（默认）：不等垂直同步

**画质优先**（录制 / 推流）：

- 发送端 `fps` 和 `bitrate_mbps` 拉高
- 发送端 `gop_frames` 给个有限值（如 120）：避免长时间只有第一帧是 IDR

## 为什么不支持环境变量

配置项需要类型和范围校验，环境变量全是字符串，加一层解析不如直接用文件。
插件配置（`--plugin-config`）是例外：那是插件自己的 schema，主程序只做透传。
