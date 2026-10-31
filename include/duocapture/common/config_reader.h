#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "duocapture/common/json.h"
#include "duocapture/common/types.h"

namespace duo {

// 按点分路径从 JSON 对象里取值，写进目标变量。
//
// 设计取舍：
//   * 不抛异常、不做链式 optional —— 读到就写、读错就记一条，最后一次性
//     报出全部问题。配置项有二十来个，修一个跑一次太折磨人。
//   * 路径不存在不是错误：文件里没写这一项，就保留结构体里的默认值
//     （或命令行已经设好的值）。
//   * 读完调用 report_unknown_keys()，把没人认领的键抓出来。拼错的键
//     （"bitrate_mbp"）如果静默忽略，用户只会觉得「配了没生效」。
class ConfigReader {
 public:
  ConfigReader(const JsonValue& root, std::string source);

  void boolean(std::string_view path, bool& out);
  void string(std::string_view path, std::string& out);

  // 整数，含范围检查。u64 版本同时覆盖 size_t（两者在所有主流平台同宽）。
  void integer(std::string_view path, u16& out, u64 min, u64 max);
  void integer(std::string_view path, u32& out, u64 min, u64 max);
  void integer(std::string_view path, u64& out, u64 min, u64 max);

  void number(std::string_view path, double& out, double min, double max);

  // 读自然单位的数字再乘 scale 存成整数，例如 bitrate_mbps → bitrate_bps。
  // min/max 约束的是文件里写的那个数，不是换算后的值。
  void scaled(std::string_view path, u32& out, double scale, double min, double max);
  void scaled(std::string_view path, u64& out, double scale, double min, double max);

  // 调用方自己做校验时上报错误（如枚举字符串不认识）。
  void fail(std::string_view path, const std::string& message);

  // 读取全部完成后调用：报告没有任何读取器认领的键。
  void report_unknown_keys();

  [[nodiscard]] bool ok() const noexcept { return errors_.empty(); }
  [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }
  [[nodiscard]] std::string joined_errors() const;

 private:
  const JsonValue* lookup(std::string_view path);
  void take_integer(std::string_view path, u64 min, u64 max, u64& out);
  void report_error(const JsonValue& at, std::string_view path, const std::string& message);
  void collect_unknown(const JsonValue& value, const std::string& prefix,
                       std::vector<std::string>& out) const;

  const JsonValue& root_;
  std::string source_;
  std::set<std::string> consumed_;
  std::vector<std::string> errors_;
};

}  // namespace duo
