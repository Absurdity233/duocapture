#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "duocapture/common/status.h"

namespace duo {

// 最小 JSON 读取器。只做解析，不做序列化——项目里唯一的用途是读配置文件，
// 而配置文件的写出交给用户自己（编辑器 / 手写）。
//
// 为什么不用第三方库：整个项目零外部依赖（nv-codec-headers 只有头文件），
// 为了读 20 个配置项引一个 500 行的库不划算，而且配置解析是安全边界之一
// （用户可能从网上下载别人的配置），自己写的一百来行反而更容易审。
//
// 严格模式：拒绝注释、尾逗号、单引号、NaN/Inf、重复键。配置文件不是
// 编程语言，松一点的解析只会让错误潜伏到运行时。
enum class JsonType { Null, Bool, Number, String, Array, Object };

class JsonValue {
 public:
  JsonType type = JsonType::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;                                       // String
  std::vector<JsonValue> elements;                        // Array
  std::vector<std::pair<std::string, JsonValue>> members; // Object
  // 该值在源文件中的位置（1 起始）。报错时指到这里，比只说键名有用得多。
  int line = 0;
  int column = 0;

  [[nodiscard]] bool is_null() const noexcept { return type == JsonType::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type == JsonType::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return type == JsonType::Number; }
  [[nodiscard]] bool is_string() const noexcept { return type == JsonType::String; }
  [[nodiscard]] bool is_array() const noexcept { return type == JsonType::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type == JsonType::Object; }

  // 对象里查找成员；不在或自己不是对象时返回 nullptr。
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  [[nodiscard]] const char* type_name() const noexcept;
};

struct JsonError {
  std::string source;  // 文件路径（解析字符串时为空）
  int line = 0;
  int column = 0;
  std::string message;

  [[nodiscard]] std::string to_string() const;
};

// 解析 JSON 文本。失败时 err 给出位置与原因。
bool parse_json(std::string_view text, JsonValue& out, JsonError& err);

// 读文件并解析。IoError = 读不到文件，InvalidArgument = 语法错误。
Status load_json_file(const std::string& path, JsonValue& out, JsonError& err);

}  // namespace duo
