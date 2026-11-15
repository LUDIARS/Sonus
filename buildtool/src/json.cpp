#include "snd/buildtool/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace snd::buildtool::json {
namespace {

class Parser {
 public:
  Parser(std::string_view text, ParseError* err) : s_(text), err_(err) {}

  bool Document(Value* out) {
    SkipWs();
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    if (pos_ != s_.size()) return Fail("unexpected trailing characters");
    return true;
  }

 private:
  static constexpr int kMaxDepth = 256;

  bool Fail(const std::string& msg) {
    if (err_ != nullptr) {
      err_->line = line_;
      err_->column = col_;
      err_->message = msg;
    }
    return false;
  }
  bool AtEnd() const { return pos_ >= s_.size(); }
  char Peek() const { return AtEnd() ? '\0' : s_[pos_]; }
  void Advance() {
    if (AtEnd()) return;
    if (s_[pos_] == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++pos_;
  }
  void SkipWs() {
    while (!AtEnd()) {
      const char c = Peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        Advance();
      } else {
        break;
      }
    }
  }
  bool Expect(char c) {
    if (Peek() != c) return Fail(std::string("expected '") + c + "'");
    Advance();
    return true;
  }
  bool Literal(const char* word) {
    for (const char* p = word; *p != '\0'; ++p) {
      if (Peek() != *p) return Fail(std::string("invalid literal, expected '") + word + "'");
      Advance();
    }
    return true;
  }

  bool ParseValue(Value* v, int depth) {
    if (depth > kMaxDepth) return Fail("nesting too deep");
    v->line = line_;
    v->column = col_;
    switch (Peek()) {
      case '{': return ParseObject(v, depth);
      case '[': return ParseArray(v, depth);
      case '"':
        v->type = Type::String;
        return ParseString(&v->text);
      case 't':
        v->type = Type::Bool;
        v->boolean = true;
        return Literal("true");
      case 'f':
        v->type = Type::Bool;
        v->boolean = false;
        return Literal("false");
      case 'n':
        v->type = Type::Null;
        return Literal("null");
      default:
        if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) return ParseNumber(v);
        if (AtEnd()) return Fail("unexpected end of input");
        return Fail(std::string("unexpected character '") + Peek() + "'");
    }
  }

  bool ParseObject(Value* v, int depth) {
    v->type = Type::Object;
    Advance();
    SkipWs();
    if (Peek() == '}') {
      Advance();
      return true;
    }
    for (;;) {
      SkipWs();
      if (Peek() != '"') return Fail("expected a member name");
      std::string key;
      const std::uint32_t kl = line_;
      const std::uint32_t kc = col_;
      if (!ParseString(&key)) return false;
      for (const auto& m : v->members) {
        if (m.first == key) {
          line_ = kl;
          col_ = kc;
          return Fail("duplicate member \"" + key + "\"");
        }
      }
      SkipWs();
      if (!Expect(':')) return false;
      SkipWs();
      Value child;
      if (!ParseValue(&child, depth + 1)) return false;
      v->members.emplace_back(std::move(key), std::move(child));
      SkipWs();
      if (Peek() == ',') {
        Advance();
        continue;
      }
      if (Peek() == '}') {
        Advance();
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(Value* v, int depth) {
    v->type = Type::Array;
    Advance();
    SkipWs();
    if (Peek() == ']') {
      Advance();
      return true;
    }
    for (;;) {
      SkipWs();
      Value child;
      if (!ParseValue(&child, depth + 1)) return false;
      v->items.push_back(std::move(child));
      SkipWs();
      if (Peek() == ',') {
        Advance();
        continue;
      }
      if (Peek() == ']') {
        Advance();
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  static void AppendUtf8(std::string* out, std::uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(std::uint32_t* out) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = Peek();
      std::uint32_t d;
      if (c >= '0' && c <= '9') {
        d = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Fail("invalid \\u escape");
      }
      v = v * 16 + d;
      Advance();
    }
    *out = v;
    return true;
  }

  bool ParseString(std::string* out) {
    Advance();  // opening quote
    out->clear();
    for (;;) {
      if (AtEnd()) return Fail("unterminated string");
      const char c = Peek();
      if (c == '"') {
        Advance();
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out->push_back(c);
        Advance();
        continue;
      }
      Advance();
      const char e = Peek();
      Advance();
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (Peek() != '\\') return Fail("unpaired surrogate");
            Advance();
            if (Peek() != 'u') return Fail("unpaired surrogate");
            Advance();
            std::uint32_t lo = 0;
            if (!Hex4(&lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return Fail("invalid surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("invalid escape");
      }
    }
  }

  bool ParseNumber(Value* v) {
    v->type = Type::Number;
    const std::size_t start = pos_;
    if (Peek() == '-') Advance();
    if (Peek() == '0') {
      Advance();
    } else if (Peek() >= '1' && Peek() <= '9') {
      while (Peek() >= '0' && Peek() <= '9') Advance();
    } else {
      return Fail("invalid number");
    }
    if (Peek() == '.') {
      Advance();
      if (!(Peek() >= '0' && Peek() <= '9')) return Fail("invalid number");
      while (Peek() >= '0' && Peek() <= '9') Advance();
    }
    if (Peek() == 'e' || Peek() == 'E') {
      Advance();
      if (Peek() == '+' || Peek() == '-') Advance();
      if (!(Peek() >= '0' && Peek() <= '9')) return Fail("invalid number");
      while (Peek() >= '0' && Peek() <= '9') Advance();
    }
    v->text = std::string(s_.substr(start, pos_ - start));
    return true;
  }

  std::string_view s_;
  ParseError* err_;
  std::size_t pos_ = 0;
  std::uint32_t line_ = 1;
  std::uint32_t col_ = 1;
};

void WriteTo(const Value& v, int indent, std::string* out) {
  const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  switch (v.type) {
    case Type::Null: *out += "null"; break;
    case Type::Bool: *out += v.boolean ? "true" : "false"; break;
    case Type::Number: *out += v.text; break;
    case Type::String: *out += Quote(v.text); break;
    case Type::Array:
      if (v.items.empty()) {
        *out += "[]";
        break;
      }
      *out += "[\n";
      for (std::size_t i = 0; i < v.items.size(); ++i) {
        *out += pad + "  ";
        WriteTo(v.items[i], indent + 1, out);
        *out += i + 1 < v.items.size() ? ",\n" : "\n";
      }
      *out += pad + "]";
      break;
    case Type::Object:
      if (v.members.empty()) {
        *out += "{}";
        break;
      }
      *out += "{\n";
      for (std::size_t i = 0; i < v.members.size(); ++i) {
        *out += pad + "  " + Quote(v.members[i].first) + ": ";
        WriteTo(v.members[i].second, indent + 1, out);
        *out += i + 1 < v.members.size() ? ",\n" : "\n";
      }
      *out += pad + "}";
      break;
  }
}

}  // namespace

const Value* Value::Find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& m : members) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

bool Value::AsDouble(double* out) const {
  if (type != Type::Number) return false;
  char* end = nullptr;
  errno = 0;
  const double d = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(d)) return false;
  *out = d;
  return true;
}

bool Value::AsU64(std::uint64_t* out) const {
  if (type != Type::Number || text.empty() || text[0] == '-') return false;
  std::uint64_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;  // fraction / exponent: not an integer literal
    const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  *out = v;
  return true;
}

bool Value::AsI64(std::int64_t* out) const {
  if (type != Type::Number || text.empty()) return false;
  const bool neg = text[0] == '-';
  std::uint64_t mag = 0;
  for (std::size_t i = neg ? 1 : 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
    if (mag > (UINT64_MAX - d) / 10) return false;
    mag = mag * 10 + d;
  }
  if (!neg && mag > static_cast<std::uint64_t>(INT64_MAX)) return false;
  if (neg && mag > static_cast<std::uint64_t>(INT64_MAX) + 1u) return false;
  *out = neg ? static_cast<std::int64_t>(0 - mag) : static_cast<std::int64_t>(mag);
  return true;
}

bool Parse(std::string_view text, Value* out, ParseError* error) {
  Parser p(text, error);
  *out = Value{};
  return p.Document(out);
}

std::string Quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(c);
        }
    }
  }
  out += "\"";
  return out;
}

std::string Write(const Value& v) {
  std::string out;
  WriteTo(v, 0, &out);
  out += "\n";
  return out;
}

Value MakeString(std::string s) {
  Value v;
  v.type = Type::String;
  v.text = std::move(s);
  return v;
}
Value MakeNumber(std::string literal) {
  Value v;
  v.type = Type::Number;
  v.text = std::move(literal);
  return v;
}
Value MakeBool(bool b) {
  Value v;
  v.type = Type::Bool;
  v.boolean = b;
  return v;
}
Value MakeArray() {
  Value v;
  v.type = Type::Array;
  return v;
}
Value MakeObject() {
  Value v;
  v.type = Type::Object;
  return v;
}
void Add(Value& object, std::string key, Value v) { object.members.emplace_back(std::move(key), std::move(v)); }

}  // namespace snd::buildtool::json
