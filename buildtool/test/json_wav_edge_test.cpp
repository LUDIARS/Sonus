// buildtool JSON reader (RFC 8259) and WAV reader: boundaries, error
// positions and fixed-seed fuzz (independent tester).
//
// Tests named Defect_* fail on the current implementation on purpose.
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "snd/buildtool/json.hpp"
#include "snd/buildtool/wav.hpp"
#include "snd/test/test.hpp"

#include <unistd.h>

#define REQUIRE_MSG(cond, ...)    \
  do {                            \
    const bool ok_ = (cond);      \
    SND_CHECK_MSG(ok_, __VA_ARGS__); \
    if (!ok_) return;             \
  } while (0)

namespace json = snd::buildtool::json;
using snd::buildtool::PcmData;
using snd::buildtool::ReadWav;

namespace {

bool Ok(const std::string& text, json::Value* v = nullptr, json::ParseError* e = nullptr) {
  json::Value local;
  json::ParseError le;
  return json::Parse(text, v != nullptr ? v : &local, e != nullptr ? e : &le);
}

std::string StrOf(const std::string& literal) {
  json::Value v;
  json::ParseError e;
  if (!json::Parse(literal, &v, &e) || !v.IsString()) return "<error>";
  return v.text;
}

bool Same(const json::Value& a, const json::Value& b) {
  if (a.type != b.type || a.boolean != b.boolean || a.text != b.text) return false;
  if (a.items.size() != b.items.size() || a.members.size() != b.members.size()) return false;
  for (std::size_t i = 0; i < a.items.size(); ++i) {
    if (!Same(a.items[i], b.items[i])) return false;
  }
  for (std::size_t i = 0; i < a.members.size(); ++i) {
    if (a.members[i].first != b.members[i].first || !Same(a.members[i].second, b.members[i].second)) return false;
  }
  return true;
}

struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed) {}
  std::uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::uint32_t Below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(Next() % n); }
};

}  // namespace

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
SND_TEST(JsonEdge, EscapesAndSurrogates) {
  SND_CHECK(StrOf("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t\"") == "\" \\ / \b \f \n \r \t");
  SND_CHECK(StrOf("\"\\u00e9\"") == "\xC3\xA9");
  SND_CHECK(StrOf("\"\\u20AC\"") == "\xE2\x82\xAC");
  SND_CHECK(StrOf("\"\\ud83d\\ude00\"") == "\xF0\x9F\x98\x80");
  SND_CHECK(StrOf("\"\\uDBFF\\uDFFF\"") == "\xF4\x8F\xBF\xBF");
  SND_CHECK(StrOf("\"\\u0000\"") == std::string(1, '\0'));
  SND_CHECK(StrOf("\"\x7F\"") == "\x7F");  // DEL is not a control character for JSON
  for (const char* bad : {"\"\\ud83d\"", "\"\\ude00\"", "\"\\ud83d\\u0041\"", "\"\\ud83dx\"", "\"\\u12\"",
                          "\"\\u12G4\"", "\"\\x41\"", "\"\\\"", "\"\\", "\"abc", "\"a\nb\"", "\"\t\"",
                          "\"\\ud83d\\", "\"\\ud83d\\ud83d\""}) {
    SND_CHECK_MSG(!Ok(bad), "accepted %s", bad);
  }
}

// RFC 8259 §8.1: JSON text exchanged between systems MUST be UTF-8. The
// project format says the files are UTF-8. Names flow into STRS, ids.json and
// the generated C++ header, so malformed UTF-8 should be rejected.
// Defect: the parser copies any byte >= 0x20 into strings unchecked.
SND_TEST(JsonEdge, Defect_InvalidUtf8InStringsIsAccepted) {
  const char* bad[] = {
      "\"\xFF\"",              // never valid
      "\"\xC0\xAF\"",          // overlong '/'
      "\"\x80\"",              // lone continuation byte
      "\"\xED\xA0\x80\"",      // encoded surrogate U+D800
      "\"\xF5\x80\x80\x80\"",  // above U+10FFFF
      "\"\xE3\x81\"",          // truncated sequence
  };
  int accepted = 0;
  for (const char* b : bad) {
    if (Ok(b)) ++accepted;
  }
  SND_CHECK_MSG(accepted == 0, "%d of 6 malformed UTF-8 strings accepted", accepted);
  SND_CHECK(Ok("\"\xE3\x81\x82\""));  // valid 'あ'
}

SND_TEST(JsonEdge, NumbersAndConversions) {
  for (const char* good : {"0", "-0", "0.5", "-0.5e-3", "1E+2", "1e2", "123456789012345678901234567890"}) {
    SND_CHECK_MSG(Ok(good), "rejected %s", good);
  }
  for (const char* bad : {"01", "1.", ".5", "+1", "-", "1e", "1e+", "--1", "0x10", "NaN", "Infinity", "-Infinity",
                          "1.e3", "- 1", "1 2"}) {
    SND_CHECK_MSG(!Ok(bad), "accepted %s", bad);
  }
  json::Value v;
  std::uint64_t u = 0;
  std::int64_t i = 0;
  double d = 0;
  SND_REQUIRE(Ok("18446744073709551615", &v));
  SND_CHECK(v.AsU64(&u) && u == UINT64_MAX);
  SND_REQUIRE(Ok("18446744073709551616", &v));
  SND_CHECK(!v.AsU64(&u));
  SND_REQUIRE(Ok("-9223372036854775808", &v));
  SND_CHECK(v.AsI64(&i) && i == INT64_MIN);
  SND_REQUIRE(Ok("-9223372036854775809", &v));
  SND_CHECK(!v.AsI64(&i));
  SND_REQUIRE(Ok("9223372036854775808", &v));
  SND_CHECK(!v.AsI64(&i));
  SND_REQUIRE(Ok("-0", &v));
  SND_CHECK(!v.AsU64(&u));
  SND_CHECK(v.AsI64(&i) && i == 0);
  SND_REQUIRE(Ok("1.0", &v));
  SND_CHECK(!v.AsU64(&u));
  SND_CHECK(v.AsDouble(&d) && d == 1.0);
  SND_REQUIRE(Ok("1e400", &v));  // valid JSON, not a finite double
  SND_CHECK(!v.AsDouble(&d));
  SND_REQUIRE(Ok("4.9e-324", &v));
  SND_CHECK(v.AsDouble(&d) && d > 0);
  SND_REQUIRE(Ok("\"5\"", &v));
  SND_CHECK(!v.AsDouble(&d) && !v.AsU64(&u));
}

// Determinism across locales (project-format.md §7): number conversion must
// not depend on LC_NUMERIC. A tool embedding the builder (the authoring GUI)
// commonly calls setlocale(LC_ALL, "").
// Defect: Value::AsDouble uses strtod, which follows LC_NUMERIC ("1.5" stops
// at '.' in de_DE and the value is rejected).
SND_TEST(JsonEdge, Defect_AsDoubleDependsOnTheLocale) {
  const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "ru_RU.UTF-8", nullptr};
  const char* used = nullptr;
  for (const char* const* l = locales; *l != nullptr && used == nullptr; ++l) {
    if (std::setlocale(LC_NUMERIC, *l) != nullptr) used = *l;
  }
  if (used == nullptr) {
    // Not installed: compile one from the glibc sources into a temp dir (LOCPATH).
    const std::string dir = "/tmp/sonus-test-locales-" + std::to_string(::getpid());
    const std::string cmd = "mkdir -p " + dir + " && localedef -i de_DE -f UTF-8 " + dir +
                            "/de_DE.UTF-8 >/dev/null 2>&1";
    if (std::system(cmd.c_str()) == 0) {
      ::setenv("LOCPATH", dir.c_str(), 1);
      if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr) used = "de_DE.UTF-8 (generated)";
    }
  }
  if (used == nullptr) {
    snd::test::Note("no comma-decimal locale available; not testable here");
    return;
  }
  json::Value v;
  double d = 0;
  SND_REQUIRE(Ok("1.5", &v));
  const bool ok = v.AsDouble(&d);
  std::setlocale(LC_NUMERIC, "C");
  SND_CHECK_MSG(ok && d == 1.5, "under %s AsDouble(\"1.5\") -> ok=%d value=%g", used, ok ? 1 : 0, d);
}

SND_TEST(JsonEdge, NestingDepthAndHugeInputs) {
  // 200 levels: fine.
  SND_CHECK(Ok(std::string(200, '[') + std::string(200, ']')));
  // Very deep input: an error, not a stack overflow.
  const std::string deep = std::string(200000, '[') + std::string(200000, ']');
  json::ParseError e;
  SND_CHECK(!Ok(deep, nullptr, &e));
  SND_CHECK(e.message.find("deep") != std::string::npos);
  const std::string deepObj = [] {
    std::string s;
    for (int i = 0; i < 100000; ++i) s += "{\"a\":";
    return s;
  }();
  SND_CHECK(!Ok(deepObj));
  // Long string and long array.
  SND_CHECK(Ok("\"" + std::string(1 << 22, 'x') + "\""));
  std::string arr = "[";
  for (int i = 0; i < 200000; ++i) arr += (i ? ",1" : "1");
  arr += "]";
  json::Value v;
  SND_CHECK(Ok(arr, &v));
  SND_CHECK_EQ(v.items.size(), 200000u);
}

// Duplicate member check is a linear scan per member: an object with many
// members takes quadratic time. Measured and reported (not a hard failure).
SND_TEST(JsonEdge, ManyMembersTime) {
  std::string obj = "{";
  const int n = 20000;
  for (int i = 0; i < n; ++i) obj += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
  obj += "}";
  const auto t0 = std::chrono::steady_clock::now();
  json::Value v;
  SND_CHECK(Ok(obj, &v));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  snd::test::Note("object with %d members parsed in %.0f ms (duplicate check is O(n^2))", n, ms);
}

SND_TEST(JsonEdge, DuplicatesTrailingCommasBomAndEmpty) {
  json::ParseError e;
  SND_CHECK(!Ok("{\"a\":1,\n \"a\":2}", nullptr, &e));
  SND_CHECK_EQ(e.line, 2u);
  SND_CHECK_EQ(e.column, 2u);
  // The same name written with an escape is the same member.
  SND_CHECK(!Ok("{\"a\":1,\"\\u0061\":2}"));
  for (const char* bad : {"[1,]", "{\"a\":1,}", "[,1]", "{,}", "", "   ", "\n", "{\"a\" 1}", "{\"a\":}", "{1:2}",
                          "[1 2]", "{\"a\":1 \"b\":2}", "nul", "tru", "falsey", "[1]]", "{}{}", "\"a\"\"b\""}) {
    SND_CHECK_MSG(!Ok(bad), "accepted '%s'", bad);
  }
  // UTF-8 BOM: RFC 8259 lets a parser ignore it; report what happens.
  const bool bom = Ok("\xEF\xBB\xBF{}");
  snd::test::Note("UTF-8 BOM before the document: %s", bom ? "accepted" : "rejected (\"unexpected character\")");
  SND_CHECK(Ok(" \t\r\n{} \t\r\n"));
}

SND_TEST(JsonEdge, ErrorPositions) {
  struct Case {
    const char* text;
    std::uint32_t line, column;
  } cases[] = {
      {"{\n  \"a\": tru\n}", 2, 11},  // where the literal stops matching
      {"[1,\n2,\n@]", 3, 1},
      {"{\"a\":1,\n\"b\"}", 2, 4},
      {"\"abc", 1, 5},              // end of input
      {"[1] x", 1, 5},
      {"{\r\n\"a\":-}", 2, 6},
  };
  for (const Case& c : cases) {
    json::ParseError e;
    SND_CHECK(!Ok(c.text, nullptr, &e));
    SND_CHECK_MSG(e.line == c.line && e.column == c.column, "'%s': got %u:%u (%s), expected %u:%u", c.text, e.line,
                  e.column, e.message.c_str(), c.line, c.column);
  }
  // Columns after multi-byte UTF-8 count bytes, not characters: report.
  json::ParseError e;
  (void)Ok("{\"\xE3\x81\x82\": x}", nullptr, &e);
  snd::test::Note("error after a 3-byte character reported at column %u (characters: 7, bytes: 9)", e.column);
  // Value positions are the first character of the value.
  json::Value v;
  SND_REQUIRE(Ok("{\n  \"k\": [\n    true\n  ]\n}", &v));
  const json::Value* k = v.Find("k");
  SND_REQUIRE(k != nullptr);
  SND_CHECK_EQ(k->line, 2u);
  SND_CHECK_EQ(k->column, 8u);
  SND_CHECK_EQ(k->items[0].line, 3u);
  SND_CHECK_EQ(k->items[0].column, 5u);
}

SND_TEST(JsonEdge, EveryPrefixOfADocumentIsRejected) {
  const std::string doc =
      "{\"type\": \"event\", \"guid\": \"{01234567-89ab-cdef-0123-456789abcdef}\", \"name\": \"SE/\\u00e9\",\n"
      " \"tracks\": [{\"clips\": [{\"asset\": \"a\", \"weight\": -1.5e-3, \"loop\": [true, false, null]}]}]}";
  SND_REQUIRE(Ok(doc));
  for (std::size_t n = 0; n < doc.size(); ++n) {
    SND_CHECK_MSG(!Ok(doc.substr(0, n)), "prefix of %zu bytes accepted", n);
  }
}

SND_TEST(JsonEdge, WriteQuoteRoundTrip) {
  std::string all;
  for (int c = 1; c < 256; ++c) all.push_back(static_cast<char>(c));
  json::Value o = json::MakeObject();
  json::Add(o, all, json::MakeString(all));
  json::Add(o, "n", json::MakeNumber("-1.25e-7"));
  json::Value back;
  SND_REQUIRE(Ok(json::Write(o), &back));
  SND_CHECK(Same(o, back));
  SND_CHECK(json::Quote(std::string(1, '\0')) == "\"\\u0000\"");
}

SND_TEST(JsonEdge, FuzzNeverCrashesAndRoundTrips) {
  const std::string seed =
      "{\"type\":\"bus\",\"guid\":\"01234567-89ab-cdef-0123-456789abcdef\",\"name\":\"A\\n\\u00e9\\ud83d\\ude00\","
      "\"sends\":[{\"bus\":\"B\",\"levelDb\":-6.5,\"preFader\":true}],\"x\":[null,false,0,-0.0e+1,{}]}";
  const char alphabet[] = "{}[],:\"\\/ubfnrt0123456789.eE+-aflsux\t\n\r \x01\x7F\xC3\xA9\xED\xFF";
  Rng rng(0xD15EA5Eull);
  std::uint32_t accepted = 0;
  for (int it = 0; it < 40000; ++it) {
    std::string s = seed;
    const std::uint32_t edits = 1 + rng.Below(4);
    for (std::uint32_t e = 0; e < edits; ++e) {
      const std::uint32_t pos = rng.Below(static_cast<std::uint32_t>(s.size()) + 1);
      const char ch = alphabet[rng.Below(sizeof(alphabet) - 1)];
      switch (rng.Below(3)) {
        case 0: if (pos < s.size()) s[pos] = ch; break;
        case 1: s.insert(s.begin() + pos, ch); break;
        default: if (pos < s.size()) s.erase(pos, 1 + rng.Below(3)); break;
      }
    }
    json::Value v;
    json::ParseError e;
    if (json::Parse(s, &v, &e)) {
      ++accepted;
      json::Value back;
      json::ParseError e2;
      SND_CHECK(json::Parse(json::Write(v), &back, &e2));
      SND_CHECK(Same(v, back));
    } else {
      SND_CHECK(!e.message.empty());
      SND_CHECK(e.line >= 1 && e.column >= 1);
    }
  }
  snd::test::Note("json fuzz: %u of 40000 accepted", accepted);
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------
namespace {

void P32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void P16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void Chunk(std::vector<std::uint8_t>& v, const char* id, const std::vector<std::uint8_t>& body, bool pad = true,
           std::int64_t sizeOverride = -1) {
  v.insert(v.end(), id, id + 4);
  P32(v, sizeOverride >= 0 ? static_cast<std::uint32_t>(sizeOverride) : static_cast<std::uint32_t>(body.size()));
  v.insert(v.end(), body.begin(), body.end());
  if (pad && (body.size() & 1u)) v.push_back(0);
}
std::vector<std::uint8_t> Fmt(std::uint16_t tag, std::uint16_t ch, std::uint32_t rate, std::uint16_t bits,
                              bool extensible = false, std::uint16_t subTag = 1) {
  std::vector<std::uint8_t> f;
  P16(f, extensible ? 0xFFFE : tag);
  P16(f, ch);
  P32(f, rate);
  P32(f, rate * ch * (bits / 8u));
  P16(f, static_cast<std::uint16_t>(ch * (bits / 8u)));
  P16(f, bits);
  if (extensible) {
    P16(f, 22);
    P16(f, bits);
    P32(f, 0x3);
    P16(f, subTag);
    const std::uint8_t tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    f.insert(f.end(), tail, tail + 14);
  }
  return f;
}
std::vector<std::uint8_t> Riff(const std::vector<std::uint8_t>& chunks) {
  std::vector<std::uint8_t> v{'R', 'I', 'F', 'F'};
  P32(v, static_cast<std::uint32_t>(chunks.size() + 4));
  v.insert(v.end(), {'W', 'A', 'V', 'E'});
  v.insert(v.end(), chunks.begin(), chunks.end());
  return v;
}

}  // namespace

SND_TEST(WavEdge, KnownSampleValuesAllFormats) {
  PcmData p;
  std::string err;
  {  // int16
    std::vector<std::uint8_t> d;
    for (std::uint16_t s : {0x7FFF, 0x8000, 0x0000, 0x4000}) P16(d, s);
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 2, 48000, 16));
    Chunk(c, "data", d);
    SND_REQUIRE(ReadWav(Riff(c), &p, &err));
    SND_CHECK_EQ(p.frames, 2u);
    SND_CHECK_NEAR(p.samples[0], 32767.0f / 32768.0f, 1e-7f);
    SND_CHECK_EQ(p.samples[1], -1.0f);
    SND_CHECK_EQ(p.samples[2], 0.0f);
    SND_CHECK_EQ(p.samples[3], 0.5f);
  }
  {  // int24, mono, odd data size (3 bytes) + pad byte + smpl after it
    std::vector<std::uint8_t> d{0x00, 0x00, 0x80};  // -8388608
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 1, 44100, 24));
    Chunk(c, "data", d);
    std::vector<std::uint8_t> smpl(36 + 24, 0);
    smpl[28] = 1;          // one loop
    smpl[36 + 8] = 0;      // start 0
    smpl[36 + 12] = 0;     // end 0 (inclusive)
    Chunk(c, "smpl", smpl);
    SND_REQUIRE(ReadWav(Riff(c), &p, &err));
    SND_CHECK_EQ(p.frames, 1u);
    SND_CHECK_EQ(p.samples[0], -1.0f);
    SND_CHECK_MSG(p.hasLoop && p.loopStart == 0 && p.loopEnd == 1, "smpl after an odd chunk not read");
  }
  {  // int24 positive full scale
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 1, 44100, 24));
    Chunk(c, "data", {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x40});
    SND_REQUIRE(ReadWav(Riff(c), &p, &err));
    SND_CHECK_NEAR(p.samples[0], 8388607.0f / 8388608.0f, 1e-7f);
    SND_CHECK_EQ(p.samples[1], 0.5f);
  }
  {  // int32
    std::vector<std::uint8_t> d;
    P32(d, 0x80000000u);
    P32(d, 0x40000000u);
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 1, 48000, 32));
    Chunk(c, "data", d);
    SND_REQUIRE(ReadWav(Riff(c), &p, &err));
    SND_CHECK_EQ(p.samples[0], -1.0f);
    SND_CHECK_EQ(p.samples[1], 0.5f);
  }
  {  // float64 (extensible) + unknown chunks before and after data
    std::vector<std::uint8_t> d(16);
    const double a = 0.25, b = -0.75;
    std::memcpy(d.data(), &a, 8);
    std::memcpy(d.data() + 8, &b, 8);
    std::vector<std::uint8_t> c;
    Chunk(c, "LIST", {1, 2, 3});  // odd, padded
    Chunk(c, "fmt ", Fmt(3, 2, 96000, 64, true, 3));
    Chunk(c, "junk", {});
    Chunk(c, "data", d);
    Chunk(c, "id3 ", {9, 9, 9, 9, 9});
    REQUIRE_MSG(ReadWav(Riff(c), &p, &err), "%s", err.c_str());
    SND_CHECK_EQ(p.channels, 2u);
    SND_CHECK_EQ(p.sampleRate, 96000u);
    SND_CHECK_EQ(p.samples[0], 0.25f);
    SND_CHECK_EQ(p.samples[1], -0.75f);
  }
  {  // 6 channels interleaved, extensible PCM 16
    std::vector<std::uint8_t> d;
    for (int f = 0; f < 3; ++f) {
      for (int ch = 0; ch < 6; ++ch) P16(d, static_cast<std::uint16_t>((ch + 1) * 1024 * (f + 1)));
    }
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 6, 48000, 16, true, 1));
    Chunk(c, "data", d);
    REQUIRE_MSG(ReadWav(Riff(c), &p, &err), "%s", err.c_str());
    SND_CHECK_EQ(p.frames, 3u);
    SND_CHECK_EQ(p.samples[2 * 6 + 5], static_cast<float>(6 * 1024 * 3) / 32768.0f);
  }
}

SND_TEST(WavEdge, UnsupportedAndMalformed) {
  PcmData p;
  std::string err;
  auto wav = [](const std::vector<std::uint8_t>& fmt, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", fmt);
    Chunk(c, "data", data);
    return Riff(c);
  };
  SND_CHECK(!ReadWav(wav(Fmt(1, 1, 48000, 8), {1, 2}), &p, &err));                // 8-bit
  SND_CHECK(!ReadWav(wav(Fmt(2, 1, 48000, 16), {1, 2}), &p, &err));               // ADPCM
  SND_CHECK(!ReadWav(wav(Fmt(1, 1, 48000, 16, true, 2), {1, 2}), &p, &err));      // extensible ADPCM
  SND_CHECK(!ReadWav(wav(Fmt(3, 1, 48000, 16), {1, 2}), &p, &err));               // float16
  SND_CHECK(!ReadWav(wav(Fmt(1, 0, 48000, 16), {}), &p, &err));                   // 0 channels
  SND_CHECK(!ReadWav(wav(Fmt(1, 1, 0, 16), {1, 2}), &p, &err));                   // rate 0
  std::vector<std::uint8_t> badAlign = Fmt(1, 2, 48000, 16);
  badAlign[12] = 3;
  SND_CHECK(!ReadWav(wav(badAlign, {1, 2, 3, 4}), &p, &err));
  // Loop outside the data.
  {
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 1, 48000, 16));
    Chunk(c, "data", {0, 0, 0, 0});
    std::vector<std::uint8_t> smpl(60, 0);
    smpl[28] = 1;
    smpl[36 + 8] = 1;
    smpl[36 + 12] = 5;
    Chunk(c, "smpl", smpl);
    SND_CHECK(!ReadWav(Riff(c), &p, &err));
  }
  // Not RIFF, too short, missing chunks.
  SND_CHECK(!ReadWav({}, &p, &err));
  SND_CHECK(!ReadWav(std::vector<std::uint8_t>{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V'}, &p, &err));
  {
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 1, 48000, 16));
    SND_CHECK(!ReadWav(Riff(c), &p, &err));
  }
  // 0 frames: accepted by the reader (report what the build does elsewhere).
  SND_CHECK(ReadWav(wav(Fmt(1, 1, 48000, 16), {}), &p, &err));
  SND_CHECK_EQ(p.frames, 0u);
}

// Size fields that lie. The reader tolerates a truncated last chunk; report
// how much data each lie yields (silent truncation is not flagged).
SND_TEST(WavEdge, LyingSizes) {
  PcmData p;
  std::string err;
  std::vector<std::uint8_t> data(64, 0x11);
  for (std::int64_t lie : {0ll, 1ll, 63ll, 65ll, 0x7FFFFFFFll, 0xFFFFFFFFll}) {
    std::vector<std::uint8_t> c;
    Chunk(c, "fmt ", Fmt(1, 1, 48000, 16));
    Chunk(c, "data", data, true, lie);
    const bool ok = ReadWav(Riff(c), &p, &err);
    snd::test::Note("data size %lld (real 64): %s, %llu frames", static_cast<long long>(lie), ok ? "accepted" : "rejected",
                    static_cast<unsigned long long>(p.frames));
  }
  // fmt size larger than the file.
  std::vector<std::uint8_t> c;
  Chunk(c, "fmt ", Fmt(1, 1, 48000, 16), true, 0xFFFFFFF0ll);
  (void)ReadWav(Riff(c), &p, &err);
}

SND_TEST(WavEdge, EveryTruncationAndFuzz) {
  std::vector<std::uint8_t> c;
  Chunk(c, "LIST", {1, 2, 3});
  Chunk(c, "fmt ", Fmt(1, 2, 48000, 24, true, 1));
  std::vector<std::uint8_t> d(6 * 20);
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(i * 37);
  Chunk(c, "data", d);
  std::vector<std::uint8_t> smpl(60, 0);
  smpl[28] = 1;
  smpl[36 + 8] = 2;
  smpl[36 + 12] = 9;
  Chunk(c, "smpl", smpl);
  const std::vector<std::uint8_t> base = Riff(c);
  PcmData p;
  std::string err;
  REQUIRE_MSG(ReadWav(base, &p, &err), "%s", err.c_str());
  for (std::size_t n = 0; n < base.size(); ++n) {
    std::vector<std::uint8_t> t(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(n));
    if (ReadWav(t, &p, &err)) {
      SND_CHECK_EQ(p.samples.size(), p.frames * p.channels);
      SND_CHECK(p.frames <= 20);
    }
  }
  Rng rng(0x3A7Eull);
  std::uint32_t accepted = 0;
  for (int it = 0; it < 40000; ++it) {
    std::vector<std::uint8_t> w = base;
    const std::uint32_t edits = 1 + rng.Below(4);
    for (std::uint32_t e = 0; e < edits; ++e) {
      const std::uint32_t pos = rng.Below(static_cast<std::uint32_t>(w.size()));
      switch (rng.Below(4)) {
        case 0: w[pos] ^= static_cast<std::uint8_t>(1u << rng.Below(8)); break;
        case 1: w[pos] = 0xFF; break;
        case 2: w[pos] = 0; break;
        default: w[pos] = static_cast<std::uint8_t>(rng.Next()); break;
      }
    }
    if (rng.Below(8) == 0) w.resize(rng.Below(static_cast<std::uint32_t>(w.size())));
    if (ReadWav(w, &p, &err)) {
      ++accepted;
      SND_CHECK_EQ(p.samples.size(), p.frames * p.channels);
      SND_CHECK(p.channels >= 1 && p.sampleRate >= 1);
      SND_CHECK(!p.hasLoop || (p.loopStart < p.loopEnd && p.loopEnd <= p.frames));
    } else {
      SND_CHECK(!err.empty());
    }
  }
  snd::test::Note("wav fuzz: %u of 40000 accepted", accepted);
}
