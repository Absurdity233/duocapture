#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

namespace duo {

// RTMP / SRT 推流。
//
// 实现方式：起一个 ffmpeg 子进程，通过**命名管道**把 MKV 喂进去，
// ffmpeg 用 `-c copy` 零转码推到 RTMP/SRT 服务端。
//
// 为什么走子进程而不是链 libavformat：
//   * libavformat 在 Windows 上要自己编译或找预编译包，依赖链很长
//   * ffmpeg 的 RTMP/SRT 实现成熟，重连、时间戳处理都现成
//   * 我们本来就在写 MKV，直接复用 MkvMuxer 的输出格式
//
// 代价是多一个进程和一次管道拷贝，但推流码率只有几 Mbps，
// 这点开销可以忽略。
class StreamPusher {
 public:
  StreamPusher();
  ~StreamPusher();

  StreamPusher(const StreamPusher&) = delete;
  StreamPusher& operator=(const StreamPusher&) = delete;

  // url 形如 rtmp://live.twitch.tv/app/<key> 或 srt://host:port?mode=caller
  // ffmpeg_path 为空时从 PATH 找。
  Status start(const std::string& url, const std::string& ffmpeg_path = "");

  // 必须在 start 之前设置：推流封装器要知道编码格式和分辨率。
  void set_video_format(Codec codec, u32 width, u32 height, u32 fps);

  // 启用音轨（必须在 start 之前）。audio_private 是 AudioSpecificConfig。
  void set_audio_format(u32 sample_rate, u16 channels, const Bytes& audio_private);

  // 推一帧 AAC（带 ADTS 头）。
  Status push_audio(ByteSpan aac_frame, u64 capture_ts_ns);

  // 推一帧（Annex-B）。内部会封装成 MKV 后写入管道。
  Status push_frame(ByteSpan frame, u64 capture_ts_ns, bool keyframe);

  Status stop();
  void abort();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] u64 frames_pushed() const noexcept;
  [[nodiscard]] u64 bytes_pushed() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 检查系统里有没有 ffmpeg。
bool ffmpeg_available(const std::string& path = "");

}  // namespace duo
