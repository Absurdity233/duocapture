#include "duocapture/common/config_reader.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace duo {
namespace {

std::string join_paths(const std::string& a, std::string_view b) {
  if (a.empty()) {
    return std::string(b);
  }
  return a + "." + std::string(b);
}

// 范围错误里带上实际值。std::to_string(double) 会给 "16.000000"，
// 对配置文件这种一眼能看懂的上下文来说太吵了。
std::string format_number(double value) {
  if (std::floor(value) == value && std::abs(value) < 1e15) {
    return std::to_string(static_cast<long long>(value));
  }
  std::string text = std::to_string(value);
  while (!text.empty() && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

std::string range_text(double min, double max) {
  return "[" + format_number(min) + ", " + format_number(max) + "]";
}

}  // namespace

ConfigReader::ConfigReader(const JsonValue& root, std::string source)
    : root_(root), source_(std::move(source)) {}

const JsonValue* ConfigReader::lookup(std::string_view path) {
  const JsonValue* node = &root_;
  std::size_t begin = 0;
  std::string prefix;

  // 沿途每一层都记进 consumed_：这样 "fec": true 这种「本该是对象却写成
  // 标量」的情况只报一次类型错误，不会再被未知键扫描重复报一遍。
  while (true) {
    const std::size_t dot = path.find('.', begin);
    const std::string_view key =
        path.substr(begin, dot == std::string_view::npos ? std::string_view::npos : dot - begin);

    if (!node->is_object()) {
      // 上一轮已经下到标量了，没法再往里找。
      report_error(*node, prefix, std::string("应该是对象，实际是") + node->type_name());
      return nullptr;
    }

    const JsonValue* child = node->find(key);
    prefix = join_paths(prefix, key);
    consumed_.insert(prefix);
    if (child == nullptr) {
      return nullptr;  // 没写这一项，保留默认值
    }
    if (dot == std::string_view::npos) {
      return child;
    }
    node = child;
    begin = dot + 1;
  }
}

void ConfigReader::report_error(const JsonValue& at, std::string_view path,
                                const std::string& message) {
  std::string line = source_;
  if (!line.empty()) {
    line += ':';
  }
  line += std::to_string(at.line) + ":" + std::to_string(at.column) + ": ";
  line += '"';
  line += path;
  line += "\" ";
  line += message;
  errors_.push_back(std::move(line));
}

void ConfigReader::boolean(std::string_view path, bool& out) {
  const JsonValue* value = lookup(path);
  if (value == nullptr) {
    return;
  }
  if (!value->is_bool()) {
    report_error(*value, path, std::string("应为 true/false，实际是") + value->type_name());
    return;
  }
  out = value->boolean;
}

void ConfigReader::string(std::string_view path, std::string& out) {
  const JsonValue* value = lookup(path);
  if (value == nullptr) {
    return;
  }
  if (!value->is_string()) {
    report_error(*value, path, std::string("应为字符串，实际是") + value->type_name());
    return;
  }
  out = value->text;
}

void ConfigReader::take_integer(std::string_view path, u64 min, u64 max, u64& out) {
  const JsonValue* value = lookup(path);
  if (value == nullptr) {
    return;
  }
  if (!value->is_number()) {
    report_error(*value, path, std::string("应为整数，实际是") + value->type_name());
    return;
  }
  const double number = value->number;
  if (!std::isfinite(number) || std::floor(number) != number) {
    report_error(*value, path, "应为整数");
    return;
  }
  if (number < static_cast<double>(min) || number > static_cast<double>(max)) {
    report_error(*value, path,
                 "超出允许范围 " + range_text(static_cast<double>(min), static_cast<double>(max)) +
                     "，实际 " + format_number(number));
    return;
  }
  out = static_cast<u64>(number);
}

void ConfigReader::integer(std::string_view path, u16& out, u64 min, u64 max) {
  u64 value = out;
  take_integer(path, min, max, value);
  out = static_cast<u16>(value);
}

void ConfigReader::integer(std::string_view path, u32& out, u64 min, u64 max) {
  u64 value = out;
  take_integer(path, min, max, value);
  out = static_cast<u32>(value);
}

void ConfigReader::integer(std::string_view path, u64& out, u64 min, u64 max) {
  u64 value = out;
  take_integer(path, min, max, value);
  out = value;
}

void ConfigReader::number(std::string_view path, double& out, double min, double max) {
  const JsonValue* value = lookup(path);
  if (value == nullptr) {
    return;
  }
  if (!value->is_number()) {
    report_error(*value, path, std::string("应为数字，实际是") + value->type_name());
    return;
  }
  if (value->number < min || value->number > max) {
    report_error(*value, path, "超出允许范围 " + range_text(min, max));
    return;
  }
  out = value->number;
}

namespace {

// 把已通过范围检查的数值换算成整数并检查是否可表示。
template <typename T>
void take_scaled(ConfigReader* self, std::string_view path, const JsonValue& value, double scale,
                 T& out) {
  const double scaled_value = std::round(value.number * scale);
  if (scaled_value < 0.0 ||
      scaled_value > static_cast<double>(std::numeric_limits<T>::max())) {
    self->fail(path, "换算后的数值超出可表示范围");
    return;
  }
  out = static_cast<T>(scaled_value);
}

}  // namespace

void ConfigReader::scaled(std::string_view path, u32& out, double scale, double min, double max) {
  const JsonValue* value = lookup(path);
  if (value == nullptr) {
    return;
  }
  if (!value->is_number()) {
    report_error(*value, path, std::string("应为数字，实际是") + value->type_name());
    return;
  }
  if (value->number < min || value->number > max) {
    report_error(*value, path, "超出允许范围 " + range_text(min, max));
    return;
  }
  take_scaled(this, path, *value, scale, out);
}

void ConfigReader::scaled(std::string_view path, u64& out, double scale, double min, double max) {
  const JsonValue* value = lookup(path);
  if (value == nullptr) {
    return;
  }
  if (!value->is_number()) {
    report_error(*value, path, std::string("应为数字，实际是") + value->type_name());
    return;
  }
  if (value->number < min || value->number > max) {
    report_error(*value, path, "超出允许范围 " + range_text(min, max));
    return;
  }
  take_scaled(this, path, *value, scale, out);
}

void ConfigReader::fail(std::string_view path, const std::string& message) {
  const JsonValue* value = lookup(path);
  if (value != nullptr) {
    report_error(*value, path, message);
  } else {
    errors_.push_back(source_ + ": \"" + std::string(path) + "\" " + message);
  }
}

void ConfigReader::collect_unknown(const JsonValue& value, const std::string& prefix,
                                   std::vector<std::string>& out) const {
  if (!value.is_object()) {
    return;
  }
  for (const auto& [key, child] : value.members) {
    const std::string path = join_paths(prefix, key);
    if (child.is_object()) {
      collect_unknown(child, path, out);
    } else if (consumed_.find(path) == consumed_.end()) {
      out.push_back(path);
    }
  }
}

void ConfigReader::report_unknown_keys() {
  std::vector<std::string> unknown;
  collect_unknown(root_, std::string(), unknown);
  for (const std::string& path : unknown) {
    std::string message = source_;
    if (!message.empty()) {
      message += ':';
    }
    message += " 未知配置项 \"";
    message += path;
    message += "\"";
    errors_.push_back(std::move(message));
  }
}

std::string ConfigReader::joined_errors() const {
  std::string out;
  for (const std::string& error : errors_) {
    out += "  ";
    out += error;
    out += '\n';
  }
  return out;
}

}  // namespace duo
