#include "duocapture/pipeline/config_file.h"

#include <string_view>

#include "duocapture/common/config_reader.h"
#include "duocapture/common/json.h"

namespace duo {
namespace {

Status load_root(const std::string& path, JsonValue& root, std::string& error) {
  JsonError json_error;
  const Status status = load_json_file(path, root, json_error);
  if (!ok(status)) {
    error = json_error.to_string() + "\n";
    return status;
  }
  if (!root.is_object()) {
    error = path + ": 顶层必须是 JSON 对象（形如 { \"fps\": 120 }）\n";
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

Status finish(const ConfigReader& reader, std::string& error) {
  if (reader.ok()) {
    return Status::Ok;
  }
  error = reader.joined_errors();
  return Status::InvalidArgument;
}

// 枚举类配置项：先按字符串读，再映射。写错时给出可选值，避免用户翻文档。
void read_codec(ConfigReader& reader, std::string_view path, Codec& out) {
  std::string text;
  reader.string(path, text);
  if (text.empty()) {
    return;  // 没写这一项
  }
  if (text == "h264") {
    out = Codec::H264;
  } else if (text == "hevc") {
    out = Codec::Hevc;
  } else if (text == "av1") {
    out = Codec::Av1;
  } else {
    reader.fail(path, "只能是 \"h264\" / \"hevc\" / \"av1\"，实际是 \"" + text + "\"");
  }
}

}  // namespace

Status apply_sender_config_file(const std::string& path, SenderConfig& config,
                                std::string& error) {
  JsonValue root;
  const Status status = load_root(path, root, error);
  if (!ok(status)) {
    return status;
  }

  ConfigReader reader(root, path);

  reader.string("target_host", config.target_host);
  reader.integer("target_port", config.target_port, 1, 65535);
  reader.integer("display_index", config.display_index, 0, 15);
  reader.boolean("prefer_wgc", config.prefer_wgc);

  reader.integer("fps", config.fps, 1, 240);
  // 码率按 Mbps 写，和人看的单位一致；内部是 bps。
  reader.scaled("bitrate_mbps", config.bitrate_bps, 1e6, 0.1, 2000.0);
  read_codec(reader, "codec", config.codec);

  reader.boolean("fec.enabled", config.enable_fec);
  // 上限 64：recover_group 用 u64 位图记录组内槽位，再多就表示不了。
  reader.integer("fec.group_size", config.fec_group_size, 2, 64);

  reader.boolean("intra_refresh", config.intra_refresh);
  reader.integer("gop_frames", config.gop_frames, 0, 1'000'000);
  // Mbit/s → bytes/s；0 表示按码率自动留余量。
  reader.scaled("pacer_mbps", config.pacer_bytes_per_second, 125'000.0, 0.0, 10'000.0);

  reader.string("record_path", config.record_path);
  reader.string("push_url", config.push_url);
  reader.string("ffmpeg_path", config.ffmpeg_path);

  reader.integer("control_port", config.control_port, 1, 65535);
  reader.integer("retransmit_window_ms", config.retransmit_window_ms, 0, 5000);

  reader.boolean("audio.enabled", config.capture_audio);
  reader.scaled("audio.bitrate_kbps", config.audio_bitrate_bps, 1000.0, 8.0, 512.0);

  reader.string("playback.path", config.playback_path);
  reader.integer("playback.fps", config.playback_fps, 1, 240);

  reader.report_unknown_keys();
  return finish(reader, error);
}

Status apply_receiver_config_file(const std::string& path, ReceiverConfig& config,
                                  std::string& error) {
  JsonValue root;
  const Status status = load_root(path, root, error);
  if (!ok(status)) {
    return status;
  }

  ConfigReader reader(root, path);

  reader.integer("listen_port", config.listen_port, 1, 65535);
  reader.integer("width", config.width, 0, 8192);
  reader.integer("height", config.height, 0, 8192);

  reader.integer("jitter_window", config.jitter_window, 1, 256);
  // 初始 NACK 延迟；自适应开关单独一项，避免「0 到底是关掉还是钉住」的歧义。
  reader.integer("nack_delay_ms", config.nack_delay_ms, 0, 200);
  reader.boolean("nack_delay_adaptive", config.nack_delay_adaptive);

  reader.boolean("show_window", config.show_window);
  reader.boolean("vsync", config.vsync);
  reader.boolean("analyze", config.enable_analysis);
  reader.boolean("play_audio", config.play_audio);
  reader.boolean("overlay", config.show_overlay);
  reader.boolean("boxes_overlay", config.boxes_overlay);

  reader.string("peer_host", config.peer_host);
  reader.integer("peer_control_port", config.peer_control_port, 1, 65535);

  reader.report_unknown_keys();
  return finish(reader, error);
}

}  // namespace duo
