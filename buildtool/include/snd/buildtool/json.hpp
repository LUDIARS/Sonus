// Minimal JSON reader/writer for the project format (no dependencies, no
// exceptions). Every value remembers its line and column for diagnostics.
//
// Numbers keep their source text so that 64-bit integers survive exactly
// (frame positions, GUID halves); AsDouble / AsU64 / AsI64 convert on demand.
#ifndef SND_BUILDTOOL_JSON_HPP
#define SND_BUILDTOOL_JSON_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snd::buildtool::json {

enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

struct Member;

struct Value {
  Type type = Type::Null;
  bool boolean = false;
  std::string text;  // String: decoded UTF-8; Number: the literal as written
  std::vector<Value> items;                            // Array
  std::vector<Member> members;  // Object, in file order
  std::uint32_t line = 0;
  std::uint32_t column = 0;

  bool IsNull() const { return type == Type::Null; }
  bool IsObject() const { return type == Type::Object; }
  bool IsArray() const { return type == Type::Array; }
  bool IsString() const { return type == Type::String; }
  bool IsNumber() const { return type == Type::Number; }
  bool IsBool() const { return type == Type::Bool; }

  // Object member or nullptr.
  const Value* Find(std::string_view key) const;

  // Conversions (false if the type or range does not fit).
  bool AsDouble(double* out) const;
  bool AsU64(std::uint64_t* out) const;  // non-negative integer literal
  bool AsI64(std::int64_t* out) const;   // integer literal
};

// Object member (pair-like names so callers read .first / .second).
struct Member {
  std::string first;
  Value second;
};

struct ParseError {
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string message;
};

// Parses one JSON document (RFC 8259; duplicate keys are an error).
bool Parse(std::string_view text, Value* out, ParseError* error);

// Serialises with 2-space indentation and the members in stored order.
// Strings and member names are written as UTF-8: well-formed sequences are
// copied, and each byte that does not start a well-formed sequence (RFC 3629:
// no overlongs, surrogates or code points above U+10FFFF) is replaced by
// U+FFFD, so Write never produces text that Parse rejects.
std::string Write(const Value& v);
// JSON string literal (quoted, escaped; malformed UTF-8 -> U+FFFD as in Write).
std::string Quote(std::string_view s);

// Builders for writers.
Value MakeString(std::string s);
Value MakeNumber(std::string literal);
Value MakeBool(bool b);
Value MakeArray();
Value MakeObject();
void Add(Value& object, std::string key, Value v);

}  // namespace snd::buildtool::json

#endif  // SND_BUILDTOOL_JSON_HPP
