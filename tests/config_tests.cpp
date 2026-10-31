// JSON 解析器与配置文件映射的回归测试。
//
// 配置文件是用户手写的，解析器的「宽容度」直接决定用户能不能看懂自己的
// 错误。所以这里重点测失败路径：错在哪一行、有没有把拼错的键抓出来、
// 类型写错时是不是保留了默认值而不是写进去一半。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "duocapture/common/config_reader.h"
#include "duocapture/common/json.h"
#include "duocapture/pipeline/config_file.h"
#include "duocapture/pipeline/receiver_config.h"
#include "duocapture/pipeline/sender_config.h"
#include "testfw.h"

namespace {

duo::JsonValue parse_ok(const std::string& text) {
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(duo::parse_json(text, root, error));
  return root;
}

// 把一段 JSON 写进临时文件，返回路径。
std::string write_temp(const std::string& name, const std::string& text) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("duocapture_test_" + name + ".json");
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  file.close();
  return path.string();
}

}  // namespace

DUO_TEST(json_parses_nested_object) {
  const duo::JsonValue root = parse_ok(R"({
    "fec": { "enabled": true, "group_size": 16 },
    "codec": "hevc",
    "list": [1, 2.5, -3, "x", null]
  })");

  DUO_CHECK(root.is_object());
  const duo::JsonValue* fec = root.find("fec");
  DUO_CHECK(fec != nullptr && fec->is_object());
  DUO_CHECK(fec->find("enabled")->boolean);
  DUO_CHECK_EQ(fec->find("group_size")->number, 16.0);

  const duo::JsonValue* list = root.find("list");
  DUO_CHECK(list != nullptr && list->is_array());
  DUO_CHECK_EQ(list->elements.size(), static_cast<std::size_t>(5));
  DUO_CHECK_EQ(list->elements[1].number, 2.5);
  DUO_CHECK_EQ(list->elements[2].number, -3.0);
  DUO_CHECK(list->elements[4].is_null());
}

DUO_TEST(json_decodes_escapes) {
  const duo::JsonValue root = parse_ok(R"({"a": "x\n\"y\"\t\\", "b": "你好"})");
  DUO_CHECK_EQ(root.find("a")->text, std::string("x\n\"y\"\t\\"));
  // 非 ASCII 字节原样透传，不做二次编码。
  DUO_CHECK_EQ(root.find("b")->text, std::string("\xE4\xBD\xA0\xE5\xA5\xBD"));
}

DUO_TEST(json_decodes_surrogate_pair) {
  // U+1F600 在 JSON 里写成代理对 D83D DE00，解析后应是 4 字节 UTF-8。
  const std::string json = "{\"e\": \"\\uD83D\\uDE00\"}";
  const duo::JsonValue root = parse_ok(json);
  DUO_CHECK_EQ(root.find("e")->text, std::string("\xF0\x9F\x98\x80"));
}

DUO_TEST(json_rejects_lone_surrogate) {
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(!duo::parse_json("{\"e\": \"\\uD83D\"}", root, error));
  DUO_CHECK(error.message.find("代理") != std::string::npos);
}

DUO_TEST(json_rejects_trailing_comma) {
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(!duo::parse_json("{\n  \"a\": 1,\n}", root, error));
  DUO_CHECK_EQ(error.line, 3);
  DUO_CHECK(error.message.find("尾逗号") != std::string::npos);
}

DUO_TEST(json_rejects_duplicate_key) {
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(!duo::parse_json(R"({"fps": 60, "fps": 120})", root, error));
  DUO_CHECK(error.message.find("重复的键") != std::string::npos);
}

DUO_TEST(json_rejects_bad_numbers) {
  const char* bad[] = {"{\"a\": 01}", "{\"a\": 1.}", "{\"a\": -}", "{\"a\": .5}", "{\"a\": 1e}"};
  for (const char* text : bad) {
    duo::JsonValue root;
    duo::JsonError error;
    DUO_CHECK(!duo::parse_json(text, root, error));
    DUO_CHECK(!error.message.empty());
  }
}

DUO_TEST(json_reports_position_on_type_error) {
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(!duo::parse_json("{\n  \"a\": tru\n}", root, error));
  DUO_CHECK_EQ(error.line, 2);
  DUO_CHECK_EQ(error.column, 8);
}

DUO_TEST(json_limits_nesting_depth) {
  std::string deep(200, '[');
  deep += std::string(200, ']');
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(!duo::parse_json(deep, root, error));
  DUO_CHECK(error.message.find("嵌套层数") != std::string::npos);
}

DUO_TEST(json_accepts_utf8_bom) {
  // Windows 记事本默认会给 UTF-8 加 BOM。
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK(duo::parse_json("\xEF\xBB\xBF{\"a\": 1}", root, error));
}

DUO_TEST(json_missing_file_is_io_error) {
  duo::JsonValue root;
  duo::JsonError error;
  DUO_CHECK_EQ(duo::load_json_file("no_such_file_duocapture.json", root, error),
               duo::Status::IoError);
}

DUO_TEST(sender_config_applies_all_sections) {
  const std::string path = write_temp("sender_full", R"({
    "target_host": "192.168.10.2",
    "target_port": 51000,
    "fps": 60,
    "bitrate_mbps": 25,
    "codec": "h264",
    "fec": { "enabled": false, "group_size": 8 },
    "audio": { "enabled": true, "bitrate_kbps": 96 },
    "record_path": "out.mkv",
    "playback": { "path": "stream.h265", "fps": 30 }
  })");

  duo::SenderConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error), duo::Status::Ok);

  DUO_CHECK_EQ(config.target_host, std::string("192.168.10.2"));
  DUO_CHECK_EQ(config.target_port, static_cast<duo::u16>(51000));
  DUO_CHECK_EQ(config.fps, 60u);
  DUO_CHECK_EQ(config.bitrate_bps, 25'000'000u);
  DUO_CHECK(config.codec == duo::Codec::H264);
  DUO_CHECK(!config.enable_fec);
  DUO_CHECK_EQ(config.fec_group_size, static_cast<std::size_t>(8));
  DUO_CHECK(config.capture_audio);
  DUO_CHECK_EQ(config.audio_bitrate_bps, 96'000u);
  DUO_CHECK_EQ(config.record_path, std::string("out.mkv"));
  DUO_CHECK_EQ(config.playback_path, std::string("stream.h265"));
  DUO_CHECK_EQ(config.playback_fps, 30u);

  // 文件里没写的项保持默认值
  DUO_CHECK_EQ(config.retransmit_window_ms, 200u);
  DUO_CHECK_EQ(config.control_port, duo::kDefaultControlPort);

  std::filesystem::remove(path);
}

DUO_TEST(sender_config_reports_unknown_key) {
  const std::string path = write_temp("sender_unknown", R"({
    "fps": 60,
    "bitrate_mbp": 10
  })");

  duo::SenderConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error), duo::Status::InvalidArgument);
  DUO_CHECK(error.find("bitrate_mbp") != std::string::npos);
  // 能读的部分仍然生效，报错只是阻止启动
  DUO_CHECK_EQ(config.fps, 60u);

  std::filesystem::remove(path);
}

DUO_TEST(sender_config_reports_type_error_without_half_writing) {
  const std::string path = write_temp("sender_type", R"({ "fps": "120" })");

  duo::SenderConfig config;
  const duo::u32 before = config.fps;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error), duo::Status::InvalidArgument);
  DUO_CHECK(error.find("fps") != std::string::npos);
  DUO_CHECK_EQ(config.fps, before);

  std::filesystem::remove(path);
}

DUO_TEST(sender_config_reports_out_of_range) {
  const std::string path = write_temp("sender_range", R"({ "fps": 5000 })");

  duo::SenderConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error), duo::Status::InvalidArgument);
  DUO_CHECK(error.find("fps") != std::string::npos);

  std::filesystem::remove(path);
}

DUO_TEST(sender_config_rejects_unknown_codec) {
  const std::string path = write_temp("sender_codec", R"({ "codec": "vp9" })");

  duo::SenderConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error), duo::Status::InvalidArgument);
  DUO_CHECK(error.find("h264") != std::string::npos);

  std::filesystem::remove(path);
}

DUO_TEST(receiver_config_maps_vsync_and_toggles) {
  const std::string path = write_temp("receiver_full", R"({
    "listen_port": 52000,
    "jitter_window": 24,
    "nack_delay_ms": 0,
    "vsync": true,
    "play_audio": false,
    "boxes_overlay": false,
    "peer_host": "192.168.10.1"
  })");

  duo::ReceiverConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_receiver_config_file(path, config, error), duo::Status::Ok);

  DUO_CHECK_EQ(config.listen_port, static_cast<duo::u16>(52000));
  DUO_CHECK_EQ(config.jitter_window, static_cast<std::size_t>(24));
  DUO_CHECK_EQ(config.nack_delay_ms, 0u);
  DUO_CHECK(config.vsync);
  DUO_CHECK(!config.play_audio);
  DUO_CHECK(!config.boxes_overlay);
  DUO_CHECK_EQ(config.peer_host, std::string("192.168.10.1"));

  std::filesystem::remove(path);
}

DUO_TEST(config_rejects_non_object_root) {
  const std::string path = write_temp("bad_root", "[1, 2, 3]");

  duo::SenderConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error),
               duo::Status::InvalidArgument);
  DUO_CHECK(error.find("顶层必须是 JSON 对象") != std::string::npos);

  std::filesystem::remove(path);
}

DUO_TEST(config_reports_multiple_errors_at_once) {
  const std::string path = write_temp("multi_error", R"({
    "fps": 0,
    "bitrate_mbps": 10,
    "unknown_thing": 1
  })");

  duo::SenderConfig config;
  std::string error;
  DUO_CHECK_EQ(duo::apply_sender_config_file(path, config, error), duo::Status::InvalidArgument);
  // 三个问题都该报出来，而不是遇到第一个就返回
  DUO_CHECK(error.find("fps") != std::string::npos);
  DUO_CHECK(error.find("unknown_thing") != std::string::npos);

  std::filesystem::remove(path);
}
