#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

namespace duo {

// 零转码 MKV 封装器。
//
// 直接把编码器吐出来的 Annex-B 码流写进 Matroska，不重新编码，所以
// 画质无损、CPU 开销接近零。要做的只是格式转换：
//
//   * Annex-B 的起始码（00 00 00 01）→ 长度前缀（hvcC/avcC 要求的 4 字节大端长度）
//   * SPS/PPS 从码流里抽出来放进 CodecPrivate（hvcC 或 avcC）
//
// 用「未知长度」写 Segment 和 Cluster，这样不需要回填文件偏移，
// 边录边写即可。VLC / ffmpeg / PotPlayer 都支持。
class MkvMuxer {
 public:
  MkvMuxer();
  ~MkvMuxer();

  MkvMuxer(const MkvMuxer&) = delete;
  MkvMuxer& operator=(const MkvMuxer&) = delete;

  // 输出目标：写文件，或把字节交给回调（推流时用回调直接进管道）。
  using Sink = void (*)(void* user, const u8* data, std::size_t size);

  // 打开输出文件。第一帧必须是关键帧（带 SPS/PPS），否则无法写轨道头。
  Status open(const std::string& path, Codec codec, u32 width, u32 height, u32 fps);

  // 打开到回调 sink。所有写出的字节按顺序交给 sink。
  Status open_sink(Sink sink, void* user, Codec codec, u32 width, u32 height, u32 fps);

  // 启用音轨（必须在 write_frame 之前调用）。audio_private 是
  // AudioSpecificConfig（2 字节）。
  Status enable_audio(u32 sample_rate, u16 channels, const Bytes& audio_private);

  // 写入一帧。frame 是 Annex-B 码流。keyframe 决定是否开新 Cluster。
  Status write_frame(ByteSpan frame, u64 capture_ts_ns, bool keyframe);

  // 写入一帧 AAC（已带 ADTS 头）。和视频共用同一条时间轴，
  // 保证音画同步。
  Status write_audio(ByteSpan aac_frame, u64 capture_ts_ns);

  Status close();
  void abort();

  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr || sink_ != nullptr; }
  [[nodiscard]] u64 frames_written() const noexcept { return frames_written_; }
  [[nodiscard]] u64 bytes_written() const noexcept { return bytes_written_; }

 private:
  Status write_header(Codec codec, u32 width, u32 height);
  Status start_cluster(u64 timecode_ms);
  void write_element(u32 id, ByteSpan payload);
  void emit(const u8* data, std::size_t size);
  void write_element_id(u32 id);
  void write_vint_size(u64 size);

  std::FILE* file_ = nullptr;
  Sink sink_ = nullptr;
  void* sink_user_ = nullptr;
  Codec codec_ = Codec::Hevc;
  u32 width_ = 0;
  u32 height_ = 0;
  u32 fps_ = 0;

  u64 timecode_scale_ns_ = 1'000'000;  // 1 ms 刻度
  u64 first_ts_ns_ = 0;
  bool have_first_ts_ = false;

  u64 cluster_start_pos_ = 0;
  u64 cluster_timecode_ = 0;
  bool cluster_open_ = false;
  u64 last_cluster_timecode_ = 0;
  u64 last_frame_timecode_ = 0;

  // 音轨
  bool audio_enabled_ = false;
  u32 audio_sample_rate_ = 0;
  u16 audio_channels_ = 0;
  Bytes audio_private_;
  u64 last_audio_timecode_ = 0;
  bool have_first_audio_ts_ = false;

  // 已写出的字节数（用于 Cues 记录 Cluster 相对位置）
  u64 bytes_written_ = 0;
  u64 frames_written_ = 0;

  std::vector<u8> nal_scratch_;
  std::vector<u8> codec_private_;
  std::vector<std::pair<u64, u64>> cue_points_;  // (timecode, cluster 相对偏移)
};

}  // namespace duo
