#include "duocapture/record/mkv_muxer.h"

#include <cstring>

#include "duocapture/common/log.h"
#include "duocapture/decode/sps_parser.h"

namespace duo {
namespace {

// ---- EBML 元素 ID（已含长度位，按原样写入）----
constexpr u32 kIdEBML = 0x1A45DFA3;
constexpr u32 kIdEBMLVersion = 0x4286;
constexpr u32 kIdEBMLReadVersion = 0x42F7;
constexpr u32 kIdEBMLMaxIDLength = 0x42F2;
constexpr u32 kIdEBMLMaxSizeLength = 0x42F3;
constexpr u32 kIdDocType = 0x4282;
constexpr u32 kIdDocTypeVersion = 0x4287;
constexpr u32 kIdDocTypeReadVersion = 0x4285;

constexpr u32 kIdSegment = 0x18538067;
constexpr u32 kIdInfo = 0x1549A966;
constexpr u32 kIdTimecodeScale = 0x2AD7B1;
constexpr u32 kIdMuxingApp = 0x4D80;
constexpr u32 kIdWritingApp = 0x5741;
constexpr u32 kIdDuration = 0x4489;

constexpr u32 kIdTracks = 0x1654AE6B;
constexpr u32 kIdTrackEntry = 0xAE;
constexpr u32 kIdTrackNumber = 0xD7;
constexpr u32 kIdTrackUID = 0x73C5;
constexpr u32 kIdTrackType = 0x83;
constexpr u32 kIdCodecID = 0x86;
constexpr u32 kIdCodecPrivate = 0x63A2;
constexpr u32 kIdDefaultDuration = 0x23E383;
constexpr u32 kIdFlagLacing = 0x9C;
constexpr u32 kIdVideo = 0xE0;
constexpr u32 kIdPixelWidth = 0xB0;
constexpr u32 kIdPixelHeight = 0xBA;

constexpr u32 kIdCluster = 0x1F43B675;
constexpr u32 kIdTimecode = 0xE7;
constexpr u32 kIdSimpleBlock = 0xA3;

constexpr u32 kIdCues = 0x1C53BB6B;
constexpr u32 kIdCuePoint = 0xBB;
constexpr u32 kIdCueTime = 0xB3;
constexpr u32 kIdCueTrackPositions = 0xB7;
constexpr u32 kIdCueTrack = 0xF7;
constexpr u32 kIdCueClusterPosition = 0xF1;

// 未知长度：1 字节 VINT，值全 1
constexpr u8 kUnknownSize1 = 0xFF;

void append_u16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

void append_u32(std::vector<u8>& out, u32 v) {
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

void append_u64(std::vector<u8>& out, u64 v) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<u8>(v >> (i * 8)));
  }
}

// 最短表示的 EBML VINT 尺寸编码
void append_vint(std::vector<u8>& out, u64 value) {
  int len = 1;
  while (len <= 8 && value >= (1ull << (7 * len))) {
    ++len;
  }
  if (len > 8) {
    len = 8;
  }
  for (int i = 0; i < len; ++i) {
    const int shift = (len - 1 - i) * 8;
    u8 byte = static_cast<u8>((value >> shift) & 0xFF);
    if (i == 0) {
      byte |= static_cast<u8>(1 << (8 - len));
    }
    out.push_back(byte);
  }
}

// 把 Annex-B 转成 4 字节长度前缀（MP4/hvcC 风格），并收集参数集。
struct ConvertedFrame {
  Bytes data;
  bool ok = false;
};

ConvertedFrame annexb_to_length_prefixed(ByteSpan frame, bool include_parameter_sets) {
  ConvertedFrame out;

  struct Nal {
    std::size_t begin;
    std::size_t end;
  };
  std::vector<Nal> nals;
  for (std::size_t i = 0; i + 4 <= frame.size(); ++i) {
    if (frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 0 && frame[i + 3] == 1) {
      nals.push_back({i + 4, frame.size()});
      if (nals.size() >= 2) {
        nals[nals.size() - 2].end = i;
      }
      i += 3;
    }
  }
  if (nals.empty()) {
    return out;
  }

  for (const Nal& nal : nals) {
    if (nal.begin >= nal.end) {
      continue;
    }
    const std::size_t len = nal.end - nal.begin;
    out.data.push_back(static_cast<u8>(len >> 24));
    out.data.push_back(static_cast<u8>(len >> 16));
    out.data.push_back(static_cast<u8>(len >> 8));
    out.data.push_back(static_cast<u8>(len));
    out.data.insert(out.data.end(), frame.begin() + static_cast<std::ptrdiff_t>(nal.begin),
                    frame.begin() + static_cast<std::ptrdiff_t>(nal.end));
  }
  (void)include_parameter_sets;
  out.ok = !out.data.empty();
  return out;
}

// 从 Annex-B 首帧里抽出 SPS/PPS，拼成 avcC（H.264）或 hvcC（HEVC）。
Bytes build_codec_private(ByteSpan first_frame, Codec codec) {
  struct Nal {
    std::size_t begin;
    std::size_t end;
  };
  std::vector<Nal> nals;
  for (std::size_t i = 0; i + 4 <= first_frame.size(); ++i) {
    if (first_frame[i] == 0 && first_frame[i + 1] == 0 && first_frame[i + 2] == 0 &&
        first_frame[i + 3] == 1) {
      nals.push_back({i + 4, first_frame.size()});
      if (nals.size() >= 2) {
        nals[nals.size() - 2].end = i;
      }
      i += 3;
    }
  }

  Bytes out;
  if (codec == Codec::H264) {
    std::vector<ByteSpan> sps_list;
    std::vector<ByteSpan> pps_list;
    for (const Nal& nal : nals) {
      if (nal.begin >= nal.end) continue;
      const u8 type = static_cast<u8>(first_frame[nal.begin] & 0x1F);
      const ByteSpan payload = first_frame.subspan(nal.begin, nal.end - nal.begin);
      if (type == 7) sps_list.push_back(payload);
      if (type == 8) pps_list.push_back(payload);
    }
    if (sps_list.empty() || pps_list.empty()) {
      return out;
    }
    const ByteSpan sps = sps_list.front();
    out.push_back(1);                       // configurationVersion
    out.push_back(sps.size() >= 1 ? sps[1] : 0x64);  // AVCProfileIndication
    out.push_back(sps.size() >= 2 ? sps[2] : 0x00);  // profile_compatibility
    out.push_back(sps.size() >= 3 ? sps[3] : 0x1F);  // AVCLevelIndication
    out.push_back(0xFF);                    // 4 字节长度前缀
    out.push_back(0xE1);                    // numOfSPS = 1
    append_u16(out, static_cast<u16>(sps.size()));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(static_cast<u8>(pps_list.size()));
    for (const ByteSpan pps : pps_list) {
      append_u16(out, static_cast<u16>(pps.size()));
      out.insert(out.end(), pps.begin(), pps.end());
    }
    return out;
  }

  // HEVC：hvcC
  std::vector<ByteSpan> vps_list;
  std::vector<ByteSpan> sps_list;
  std::vector<ByteSpan> pps_list;
  for (const Nal& nal : nals) {
    if (nal.begin >= nal.end) continue;
    const u8 type = static_cast<u8>((first_frame[nal.begin] >> 1) & 0x3F);
    const ByteSpan payload = first_frame.subspan(nal.begin, nal.end - nal.begin);
    if (type == 32) vps_list.push_back(payload);
    if (type == 33) sps_list.push_back(payload);
    if (type == 34) pps_list.push_back(payload);
  }
  if (sps_list.empty() || pps_list.empty()) {
    return out;
  }

  const ByteSpan sps = sps_list.front();
  out.push_back(1);  // configurationVersion
  // general_profile_space/tier/profile_idc
  out.push_back(sps.size() > 1 ? sps[1] : 0x01);
  append_u32(out, sps.size() > 5 ? (static_cast<u32>(sps[2]) << 24 | static_cast<u32>(sps[3]) << 16 |
                                   static_cast<u32>(sps[4]) << 8 | static_cast<u32>(sps[5]))
                                 : 0);
  for (int i = 0; i < 6; ++i) {
    out.push_back(static_cast<u8>(sps.size() > 6 + i ? sps[6 + i] : 0));  // constraint flags
  }
  out.push_back(static_cast<u8>(sps.size() > 12 ? sps[12] : 0));  // general_level_idc
  append_u16(out, 0xF000);  // min_spatial_segmentation_idc
  out.push_back(0xFC);      // parallelismType
  out.push_back(0xFC | 0x01);  // chromaFormat = 4:2:0
  out.push_back(0xF8);         // bitDepthLumaMinus8
  out.push_back(0xF8);         // bitDepthChromaMinus8
  append_u16(out, 0);          // avgFrameRate
  out.push_back(0x0F);         // constantFrameRate/numTemporalLayers/temporalIdNested/lengthSizeMinusOne
  out.push_back(static_cast<u8>(vps_list.size() + sps_list.size() + pps_list.size()));

  auto push_array = [&out](u8 nal_type, const std::vector<ByteSpan>& list) {
    if (list.empty()) return;
    out.push_back(static_cast<u8>(0x80 | nal_type));
    append_u16(out, static_cast<u16>(list.size()));
    for (const ByteSpan n : list) {
      append_u16(out, static_cast<u16>(n.size()));
      out.insert(out.end(), n.begin(), n.end());
    }
  };
  push_array(32, vps_list);
  push_array(33, sps_list);
  push_array(34, pps_list);
  return out;
}

}  // namespace

MkvMuxer::MkvMuxer() = default;

MkvMuxer::~MkvMuxer() { abort(); }

void MkvMuxer::write_element_id(u32 id) {
  // ID 已经带长度位，直接按它自身占用的字节数写。
  u8 buf[4];
  int n = 0;
  if (id & 0xFF000000) {
    buf[n++] = static_cast<u8>(id >> 24);
  }
  if (id & 0x00FF0000) {
    buf[n++] = static_cast<u8>(id >> 16);
  }
  if (id & 0x0000FF00) {
    buf[n++] = static_cast<u8>(id >> 8);
  }
  if (id & 0x000000FF) {
    buf[n++] = static_cast<u8>(id);
  }
  std::fwrite(buf, 1, static_cast<std::size_t>(n), file_);
  bytes_written_ += static_cast<u64>(n);
}

void MkvMuxer::write_vint_size(u64 size) {
  std::vector<u8> buf;
  append_vint(buf, size);
  std::fwrite(buf.data(), 1, buf.size(), file_);
  bytes_written_ += buf.size();
}

void MkvMuxer::write_element(u32 id, ByteSpan payload) {
  write_element_id(id);
  write_vint_size(payload.size());
  if (!payload.empty()) {
    std::fwrite(payload.data(), 1, payload.size(), file_);
    bytes_written_ += payload.size();
  }
}

Status MkvMuxer::open(const std::string& path, Codec codec, u32 width, u32 height, u32 fps) {
  if (file_ != nullptr) {
    return Status::InvalidArgument;
  }
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    return Status::IoError;
  }

  codec_ = codec;
  width_ = width;
  height_ = height;
  fps_ = fps == 0 ? 60 : fps;
  bytes_written_ = 0;
  frames_written_ = 0;
  have_first_ts_ = false;
  cluster_open_ = false;
  cue_points_.clear();

  Status status = write_header(codec, width, height);
  if (!ok(status)) {
    abort();
    return status;
  }
  return Status::Ok;
}

Status MkvMuxer::write_header(Codec codec, u32 width, u32 height) {
  // ---- EBML 头 ----
  {
    std::vector<u8> hdr;
    std::vector<u8> version;
    append_u16(version, 1);
    write_element_id(kIdEBML);
    // 先算长度：固定内容
    std::vector<u8> body;
    auto sub = [&body](u32 id, const std::vector<u8>& v) {
      std::vector<u8> idbuf;
      u8 b[4];
      int n = 0;
      if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
      if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
      if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
      if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
      body.insert(body.end(), b, b + n);
      append_vint(body, v.size());
      body.insert(body.end(), v.begin(), v.end());
    };

    std::vector<u8> v1;
    append_u16(v1, 1);
    sub(kIdEBMLVersion, v1);
    sub(kIdEBMLReadVersion, v1);
    std::vector<u8> v4;
    append_u16(v4, 4);
    sub(kIdEBMLMaxIDLength, v4);
    sub(kIdEBMLMaxSizeLength, v4);
    std::vector<u8> doctype{'m', 'a', 't', 'r', 'o', 's', 'k', 'a'};
    sub(kIdDocType, doctype);
    std::vector<u8> v2;
    append_u16(v2, 2);
    sub(kIdDocTypeVersion, v2);
    sub(kIdDocTypeReadVersion, v2);

    write_vint_size(body.size());
    std::fwrite(body.data(), 1, body.size(), file_);
    bytes_written_ += body.size();
    (void)hdr;
  }

  // ---- Segment（未知长度，边写边填）----
  write_element_id(kIdSegment);
  {
    u8 unknown = kUnknownSize1;
    std::fwrite(&unknown, 1, 1, file_);
    bytes_written_ += 1;
  }

  // ---- Info ----
  {
    std::vector<u8> info;
    auto sub = [&info](u32 id, const std::vector<u8>& v) {
      u8 b[4];
      int n = 0;
      if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
      if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
      if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
      if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
      info.insert(info.end(), b, b + n);
      append_vint(info, v.size());
      info.insert(info.end(), v.begin(), v.end());
    };

    std::vector<u8> scale;
    append_u32(scale, static_cast<u32>(timecode_scale_ns_));
    sub(kIdTimecodeScale, scale);
    const std::string app = "duocapture";
    sub(kIdMuxingApp, Bytes(app.begin(), app.end()));
    sub(kIdWritingApp, Bytes(app.begin(), app.end()));
    write_element(kIdInfo, ByteSpan(info.data(), info.size()));
  }

  // ---- Tracks（先占位，等第一帧拿到 codec private 再写）----
  // 这里先不写 Tracks，write_frame 第一次调用时补上。
  (void)codec;
  (void)width;
  (void)height;
  return Status::Ok;
}

Status MkvMuxer::start_cluster(u64 timecode_ms) {
  // 关闭上一个 Cluster：Cluster 也是未知长度，靠下一个 Cluster 或
  // Cues/Segment 结束来界定。MKV 解析器接受这种写法。
  write_element_id(kIdCluster);
  {
    u8 unknown = kUnknownSize1;
    std::fwrite(&unknown, 1, 1, file_);
    bytes_written_ += 1;
  }

  cluster_start_pos_ = bytes_written_;
  cluster_timecode_ = timecode_ms;
  last_cluster_timecode_ = timecode_ms;

  std::vector<u8> tc;
  append_u16(tc, static_cast<u16>(timecode_ms & 0xFFFF));
  write_element(kIdTimecode, ByteSpan(tc.data(), tc.size()));

  cluster_open_ = true;
  return Status::Ok;
}

Status MkvMuxer::write_frame(ByteSpan frame, u64 capture_ts_ns, bool keyframe) {
  if (file_ == nullptr) {
    return Status::NotInitialized;
  }
  if (!have_first_ts_) {
    first_ts_ns_ = capture_ts_ns;
    have_first_ts_ = true;
  }

  // 第一帧：此时才知道 SPS/PPS，补写 Tracks
  if (frames_written_ == 0) {
    codec_private_ = build_codec_private(frame, codec_);
    if (codec_private_.empty()) {
      DUO_ERROR("首帧里没有 SPS/PPS，无法写 MKV 轨道头");
      return Status::InvalidArgument;
    }

    std::vector<u8> tracks;
    auto sub = [&tracks](u32 id, const std::vector<u8>& v) {
      u8 b[4];
      int n = 0;
      if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
      if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
      if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
      if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
      tracks.insert(tracks.end(), b, b + n);
      append_vint(tracks, v.size());
      tracks.insert(tracks.end(), v.begin(), v.end());
    };

    std::vector<u8> entry;
    auto esub = [&entry](u32 id, const std::vector<u8>& v) {
      u8 b[4];
      int n = 0;
      if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
      if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
      if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
      if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
      entry.insert(entry.end(), b, b + n);
      append_vint(entry, v.size());
      entry.insert(entry.end(), v.begin(), v.end());
    };

    std::vector<u8> v;
    v = {1};
    esub(kIdTrackNumber, v);
    std::vector<u8> uid;
    append_u64(uid, 1);
    esub(kIdTrackUID, uid);
    v = {1};  // video
    esub(kIdTrackType, v);
    const std::string codec_id = codec_ == Codec::Hevc ? "V_MPEGH/ISO/HEVC"
                                  : codec_ == Codec::H264 ? "V_MPEG4/ISO/AVC"
                                                          : "V_AV1";
    esub(kIdCodecID, Bytes(codec_id.begin(), codec_id.end()));
    esub(kIdCodecPrivate, codec_private_);
    v = {0};
    esub(kIdFlagLacing, v);
    std::vector<u8> dur;
    append_u32(dur, static_cast<u32>(1'000'000'000ull / (fps_ == 0 ? 60 : fps_)));
    esub(kIdDefaultDuration, dur);

    std::vector<u8> video;
    auto vsub = [&video](u32 id, const std::vector<u8>& vv) {
      u8 b[4];
      int n = 0;
      if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
      if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
      if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
      if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
      video.insert(video.end(), b, b + n);
      append_vint(video, vv.size());
      video.insert(video.end(), vv.begin(), vv.end());
    };
    std::vector<u8> wbuf;
    append_u16(wbuf, static_cast<u16>(width_));
    vsub(kIdPixelWidth, wbuf);
    std::vector<u8> hbuf;
    append_u16(hbuf, static_cast<u16>(height_));
    vsub(kIdPixelHeight, hbuf);
    esub(kIdVideo, video);

    sub(kIdTrackEntry, entry);

    // 音轨（A_AAC）
    if (audio_enabled_) {
      std::vector<u8> aentry;
      auto asub = [&aentry](u32 id, const std::vector<u8>& v) {
        u8 b[4];
        int n = 0;
        if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
        if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
        if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
        if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
        aentry.insert(aentry.end(), b, b + n);
        append_vint(aentry, v.size());
        aentry.insert(aentry.end(), v.begin(), v.end());
      };
      std::vector<u8> av;
      av = {2};
      asub(kIdTrackNumber, av);
      std::vector<u8> auid;
      append_u64(auid, 2);
      asub(kIdTrackUID, auid);
      av = {2};  // audio
      asub(kIdTrackType, av);
      const std::string acodec = "A_AAC";
      asub(kIdCodecID, Bytes(acodec.begin(), acodec.end()));
      asub(kIdCodecPrivate, audio_private_);
      av = {0};
      asub(kIdFlagLacing, av);

      // Audio 元素：采样率、声道数、位深
      std::vector<u8> audio_elem;
      auto ausub = [&audio_elem](u32 id, const std::vector<u8>& v) {
        u8 b[4];
        int n = 0;
        if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
        if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
        if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
        if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
        audio_elem.insert(audio_elem.end(), b, b + n);
        append_vint(audio_elem, v.size());
        audio_elem.insert(audio_elem.end(), v.begin(), v.end());
      };
      // SamplingFrequency 是 float
      std::vector<u8> sfreq(4);
      const float f = static_cast<float>(audio_sample_rate_);
      std::memcpy(sfreq.data(), &f, 4);
      ausub(0xB5, sfreq);
      std::vector<u8> ch;
      ch.push_back(static_cast<u8>(audio_channels_));
      ausub(0x9F, ch);
      std::vector<u8> bd;
      bd.push_back(16);
      ausub(0x6264, bd);
      asub(0xE1, audio_elem);

      sub(kIdTrackEntry, aentry);
    }

    write_element(kIdTracks, ByteSpan(tracks.data(), tracks.size()));
  }

  const u64 rel_ns = capture_ts_ns - first_ts_ns_;
  u64 timecode_ms = rel_ns / timecode_scale_ns_;

  // 时间戳必须严格递增。1 ms 的刻度下，120fps 每帧间隔 8.3ms 没问题，
  // 但采集抖动或回放模式可能让两帧落进同一毫秒，产生
  // "non monotonically increasing dts" 警告。这里强制单调。
  if (timecode_ms <= last_frame_timecode_) {
    timecode_ms = last_frame_timecode_ + 1;
  }
  last_frame_timecode_ = timecode_ms;

  // 关键帧或超过 1 秒就开新 Cluster
  // 开新 Cluster 时，Cluster 时间码取当前帧时间。后续帧的相对时间戳
  // 就是 timecode_ms - cluster_timecode_，天然非负。
  if (!cluster_open_ || (keyframe && timecode_ms > last_cluster_timecode_ + 1000)) {
    start_cluster(timecode_ms);
  }

  const ConvertedFrame converted = annexb_to_length_prefixed(frame, false);
  if (!converted.ok) {
    return Status::InvalidArgument;
  }

  // SimpleBlock：track number (vint) + 2 字节相对时间戳 + flags + 数据
  std::vector<u8> block;
  append_vint(block, 1);
  const u16 rel = static_cast<u16>(static_cast<i16>(timecode_ms - cluster_timecode_));
  block.push_back(static_cast<u8>(rel >> 8));
  block.push_back(static_cast<u8>(rel));
  block.push_back(keyframe ? 0x80 : 0x00);
  block.insert(block.end(), converted.data.begin(), converted.data.end());

  write_element(kIdSimpleBlock, ByteSpan(block.data(), block.size()));
  ++frames_written_;
  return Status::Ok;
}

Status MkvMuxer::enable_audio(u32 sample_rate, u16 channels, const Bytes& audio_private) {
  if (file_ == nullptr) {
    return Status::NotInitialized;
  }
  if (frames_written_ > 0) {
    // 轨道头已经写死了，此时加不了音轨
    return Status::InvalidArgument;
  }
  audio_sample_rate_ = sample_rate;
  audio_channels_ = channels;
  audio_private_ = audio_private;
  audio_enabled_ = true;
  return Status::Ok;
}

Status MkvMuxer::write_audio(ByteSpan aac_frame, u64 capture_ts_ns) {
  if (file_ == nullptr || !audio_enabled_) {
    return Status::NotInitialized;
  }
  if (!have_first_ts_) {
    return Status::InvalidArgument;  // 必须等视频第一帧确定时间基准
  }
  if (aac_frame.size() <= 7) {
    return Status::InvalidArgument;
  }

  // 剥掉 ADTS 头（7 字节），MKV 里存裸 AAC 帧
  ByteSpan payload = aac_frame.subspan(7);

  const u64 rel_ns = capture_ts_ns > first_ts_ns_ ? capture_ts_ns - first_ts_ns_ : 0;
  u64 timecode_ms = rel_ns / timecode_scale_ns_;
  if (timecode_ms <= last_audio_timecode_) {
    timecode_ms = last_audio_timecode_ + 1;
  }
  last_audio_timecode_ = timecode_ms;

  // Cluster 只由视频关键帧切换。音频不能自己开 Cluster —— 否则视频的
  // 相对时间戳（timecode_ms - cluster_timecode_）会被音频拉成负数，
  // 表现为 DTS 重复。
  if (!cluster_open_) {
    start_cluster(timecode_ms);
  }

  std::vector<u8> block;
  append_vint(block, 2);  // track 2
  const u16 rel = static_cast<u16>(static_cast<i16>(timecode_ms - cluster_timecode_));
  block.push_back(static_cast<u8>(rel >> 8));
  block.push_back(static_cast<u8>(rel));
  block.push_back(0x80);  // keyframe
  block.insert(block.end(), payload.begin(), payload.end());

  write_element(kIdSimpleBlock, ByteSpan(block.data(), block.size()));
  return Status::Ok;
}

Status MkvMuxer::close() {
  if (file_ == nullptr) {
    return Status::NotInitialized;
  }

  // Cues：让播放器能 seek。Cluster 位置是相对 Segment 数据起点的偏移，
  // 这里用简化写法（相对文件头），主流播放器容错。
  if (!cue_points_.empty()) {
    std::vector<u8> cues;
    auto sub = [&cues](u32 id, const std::vector<u8>& v) {
      u8 b[4];
      int n = 0;
      if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
      if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
      if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
      if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
      cues.insert(cues.end(), b, b + n);
      append_vint(cues, v.size());
      cues.insert(cues.end(), v.begin(), v.end());
    };
    for (const auto& [tc, pos] : cue_points_) {
      std::vector<u8> point;
      auto psub = [&point](u32 id, const std::vector<u8>& v) {
        u8 b[4];
        int n = 0;
        if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
        if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
        if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
        if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
        point.insert(point.end(), b, b + n);
        append_vint(point, v.size());
        point.insert(point.end(), v.begin(), v.end());
      };
      std::vector<u8> tcb;
      append_u16(tcb, static_cast<u16>(tc & 0xFFFF));
      psub(kIdCueTime, tcb);

      std::vector<u8> pos_entry;
      auto qsub = [&pos_entry](u32 id, const std::vector<u8>& v) {
        u8 b[4];
        int n = 0;
        if (id & 0xFF000000) b[n++] = static_cast<u8>(id >> 24);
        if (id & 0x00FF0000) b[n++] = static_cast<u8>(id >> 16);
        if (id & 0x0000FF00) b[n++] = static_cast<u8>(id >> 8);
        if (id & 0x000000FF) b[n++] = static_cast<u8>(id);
        pos_entry.insert(pos_entry.end(), b, b + n);
        append_vint(pos_entry, v.size());
        pos_entry.insert(pos_entry.end(), v.begin(), v.end());
      };
      std::vector<u8> track;
      append_u16(track, 1);
      qsub(kIdCueTrack, track);
      std::vector<u8> cluster_pos;
      append_u64(cluster_pos, pos);
      qsub(kIdCueClusterPosition, cluster_pos);
      psub(kIdCueTrackPositions, pos_entry);

      sub(kIdCuePoint, point);
    }
    write_element(kIdCues, ByteSpan(cues.data(), cues.size()));
  }

  std::fclose(file_);
  file_ = nullptr;
  DUO_INFO("MKV 录制结束: %llu 帧, %.1f MB", static_cast<unsigned long long>(frames_written_),
           static_cast<double>(bytes_written_) / (1024.0 * 1024.0));
  return Status::Ok;
}

void MkvMuxer::abort() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace duo
