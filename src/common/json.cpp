#include "duocapture/common/json.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>

#include "duocapture/common/types.h"

namespace duo {
namespace {

// 嵌套深度上限。配置文件的合理深度是个位数；不设上限的话，
// 一份 "[[[[..." 的恶意输入能把递归下降的栈打穿。
constexpr int kMaxDepth = 32;

class Parser {
 public:
  Parser(std::string_view text, JsonError& err) : text_(text), err_(err) {}

  bool parse_document(JsonValue& out) {
    skip_whitespace();
    if (at_end()) {
      return fail("文件为空，至少需要一个 JSON 对象");
    }
    if (!parse_value(out, 0)) {
      return false;
    }
    skip_whitespace();
    if (!at_end()) {
      return fail("根值之后还有多余内容");
    }
    return true;
  }

 private:
  bool at_end() const { return pos_ >= text_.size(); }
  char peek() const { return text_[pos_]; }

  void advance() {
    if (text_[pos_] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    ++pos_;
  }

  void skip_whitespace() {
    while (!at_end()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else {
        break;
      }
    }
  }

  bool fail(const std::string& message) {
    err_.line = line_;
    err_.column = column_;
    err_.message = message;
    return false;
  }

  bool expect(char c, const char* what) {
    if (at_end() || peek() != c) {
      return fail(std::string("缺少 ") + what);
    }
    advance();
    return true;
  }

  bool parse_value(JsonValue& out, int depth) {
    if (depth > kMaxDepth) {
      return fail("嵌套层数超过上限（" + std::to_string(kMaxDepth) + "）");
    }
    if (at_end()) {
      return fail("值不完整（文件意外结束）");
    }

    out.line = line_;
    out.column = column_;

    const char c = peek();
    switch (c) {
      case '{': return parse_object(out, depth);
      case '[': return parse_array(out, depth);
      case '"': {
        out.type = JsonType::String;
        return parse_string(out.text);
      }
      case 't': return parse_literal("true", out, JsonType::Bool, true);
      case 'f': return parse_literal("false", out, JsonType::Bool, false);
      case 'n': {
        out.type = JsonType::Null;
        return parse_literal("null", out, JsonType::Null, false);
      }
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          out.type = JsonType::Number;
          return parse_number(out.number);
        }
        return fail(std::string("意外的字符 '") + c + "'");
    }
  }

  bool parse_literal(const char* literal, JsonValue& out, JsonType type, bool boolean) {
    const std::string_view text(literal);
    if (text_.substr(pos_, text.size()) != text) {
      return fail(std::string("无法识别的字面量，期望 ") + literal);
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
      advance();
    }
    out.type = type;
    out.boolean = boolean;
    return true;
  }

  bool parse_number(double& out) {
    const std::size_t begin = pos_;
    while (!at_end()) {
      const char c = peek();
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        advance();
      } else {
        break;
      }
    }

    const std::string_view token = text_.substr(begin, pos_ - begin);
    // strtod 会把空串和越界解析成 0/∞，所以先自己校验语法：
    // 只接受 [-+]?digits[.digits][(e|E)[-+]digits]，且不接受前导 '+'。
    if (!valid_number_syntax(token)) {
      return fail("无效的数字 '" + std::string(token) + "'");
    }

    const std::string owned(token);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size() || !std::isfinite(value)) {
      return fail("数字超出可表示范围 '" + owned + "'");
    }
    out = value;
    return true;
  }

  static bool valid_number_syntax(std::string_view token) {
    std::size_t i = 0;
    if (i < token.size() && token[i] == '-') {
      ++i;
    }
    // 整数部分：0 或不以 0 开头的数字串
    if (i >= token.size() || token[i] < '0' || token[i] > '9') {
      return false;
    }
    if (token[i] == '0') {
      ++i;
    } else {
      while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
        ++i;
      }
    }
    if (i < token.size() && token[i] == '.') {
      ++i;
      const std::size_t start = i;
      while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
        ++i;
      }
      if (i == start) {
        return false;
      }
    }
    if (i < token.size() && (token[i] == 'e' || token[i] == 'E')) {
      ++i;
      if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
        ++i;
      }
      const std::size_t start = i;
      while (i < token.size() && token[i] >= '0' && token[i] <= '9') {
        ++i;
      }
      if (i == start) {
        return false;
      }
    }
    return i == token.size();
  }

  bool parse_string(std::string& out) {
    out.clear();
    if (!expect('"', "起始引号")) {
      return false;
    }

    while (true) {
      if (at_end()) {
        return fail("字符串没有结束引号");
      }
      const char c = peek();
      if (c == '"') {
        advance();
        return true;
      }
      if (c == '\\') {
        advance();
        if (!parse_escape(out)) {
          return false;
        }
        continue;
      }
      // JSON 不允许字符串里出现裸控制字符（含换行）。
      if (static_cast<unsigned char>(c) < 0x20) {
        return fail("字符串里有未转义的控制字符");
      }
      out.push_back(c);
      advance();
    }
  }

  bool parse_escape(std::string& out) {
    if (at_end()) {
      return fail("转义序列不完整");
    }
    const char c = peek();
    advance();
    switch (c) {
      case '"': out.push_back('"'); return true;
      case '\\': out.push_back('\\'); return true;
      case '/': out.push_back('/'); return true;
      case 'b': out.push_back('\b'); return true;
      case 'f': out.push_back('\f'); return true;
      case 'n': out.push_back('\n'); return true;
      case 'r': out.push_back('\r'); return true;
      case 't': out.push_back('\t'); return true;
      case 'u': return parse_unicode_escape(out);
      default: return fail(std::string("未知的转义序列 '\\") + c + "'");
    }
  }

  bool parse_unicode_escape(std::string& out) {
    u32 code_point = 0;
    if (!read_hex4(code_point)) {
      return false;
    }

    // 代理对：高位 D800-DBFF 后面必须再跟一个 \uDC00-DFFF。
    // 落单的代理项直接报错，不做「宽容处理」——它几乎总是别的东西的证据。
    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
      if (text_.substr(pos_, 2) != "\\u") {
        return fail("高位代理后缺少低位代理");
      }
      advance();
      advance();
      u32 low = 0;
      if (!read_hex4(low)) {
        return false;
      }
      if (low < 0xDC00 || low > 0xDFFF) {
        return fail("低位代理不在 DC00-DFFF 范围内");
      }
      code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
    } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
      return fail("孤立的低位代理");
    }

    append_utf8(out, code_point);
    return true;
  }

  bool read_hex4(u32& out) {
    if (pos_ + 4 > text_.size()) {
      return fail("\\u 转义需要 4 位十六进制数字");
    }
    u32 value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + static_cast<std::size_t>(i)];
      u32 digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<u32>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<u32>(c - 'a') + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<u32>(c - 'A') + 10;
      } else {
        return fail("\\u 转义里有非十六进制字符");
      }
      value = (value << 4) | digit;
    }
    for (int i = 0; i < 4; ++i) {
      advance();
    }
    out = value;
    return true;
  }

  static void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_object(JsonValue& out, int depth) {
    out.type = JsonType::Object;
    advance();  // '{'
    skip_whitespace();
    if (!at_end() && peek() == '}') {
      advance();
      return true;
    }

    while (true) {
      skip_whitespace();
      if (at_end()) {
        return fail("对象没有结束的 '}'");
      }
      if (peek() != '"') {
        return fail("对象的键必须是字符串");
      }

      const int key_line = line_;
      const int key_column = column_;
      std::string key;
      if (!parse_string(key)) {
        return false;
      }

      // 重复键在标准里是「实现自定」，这里一律报错：配置项重复几乎
      // 一定是复制粘贴留下的，静默取最后一个只会让人查半天。
      if (out.find(key) != nullptr) {
        err_.line = key_line;
        err_.column = key_column;
        err_.message = "重复的键 \"" + key + "\"";
        return false;
      }

      skip_whitespace();
      if (!expect(':', "'：'")) {
        return false;
      }
      skip_whitespace();

      JsonValue value;
      if (!parse_value(value, depth + 1)) {
        return false;
      }
      out.members.emplace_back(std::move(key), std::move(value));

      skip_whitespace();
      if (at_end()) {
        return fail("对象没有结束的 '}'");
      }
      if (peek() == ',') {
        advance();
        skip_whitespace();
        // 尾逗号：{ "a": 1, } —— 明确拒绝，不给「顺手写错」留活路。
        if (!at_end() && peek() == '}') {
          return fail("对象里有多余的尾逗号");
        }
        continue;
      }
      if (peek() == '}') {
        advance();
        return true;
      }
      return fail("对象成员之间缺少 ','");
    }
  }

  bool parse_array(JsonValue& out, int depth) {
    out.type = JsonType::Array;
    advance();  // '['
    skip_whitespace();
    if (!at_end() && peek() == ']') {
      advance();
      return true;
    }

    while (true) {
      skip_whitespace();
      JsonValue value;
      if (!parse_value(value, depth + 1)) {
        return false;
      }
      out.elements.push_back(std::move(value));

      skip_whitespace();
      if (at_end()) {
        return fail("数组没有结束的 ']'");
      }
      if (peek() == ',') {
        advance();
        skip_whitespace();
        if (!at_end() && peek() == ']') {
          return fail("数组里有多余的尾逗号");
        }
        continue;
      }
      if (peek() == ']') {
        advance();
        return true;
      }
      return fail("数组元素之间缺少 ','");
    }
  }

  std::string_view text_;
  JsonError& err_;
  std::size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (type != JsonType::Object) {
    return nullptr;
  }
  for (const auto& [name, value] : members) {
    if (name == key) {
      return &value;
    }
  }
  return nullptr;
}

const char* JsonValue::type_name() const noexcept {
  switch (type) {
    case JsonType::Null: return "null";
    case JsonType::Bool: return "布尔值";
    case JsonType::Number: return "数字";
    case JsonType::String: return "字符串";
    case JsonType::Array: return "数组";
    case JsonType::Object: return "对象";
  }
  return "未知类型";
}

std::string JsonError::to_string() const {
  std::string out;
  if (!source.empty()) {
    out += source;
    out += ':';
  }
  out += std::to_string(line) + ":" + std::to_string(column) + ": " + message;
  return out;
}

bool parse_json(std::string_view text, JsonValue& out, JsonError& err) {
  // UTF-8 BOM：Windows 记事本默认会加，直接报「意外的字符」太不友好了。
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }

  out = JsonValue{};
  Parser parser(text, err);
  return parser.parse_document(out);
}

Status load_json_file(const std::string& path, JsonValue& out, JsonError& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err.source = path;
    err.line = 0;
    err.column = 0;
    err.message = "无法打开文件";
    return Status::IoError;
  }

  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) {
    err.source = path;
    err.line = 0;
    err.column = 0;
    err.message = "读取文件失败";
    return Status::IoError;
  }

  if (!parse_json(text, out, err)) {
    err.source = path;
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

}  // namespace duo
