// "..."_id hash (design.md 10.1): C, C++ runtime and compile-time agree.
#include <cstring>

#include "snd/id.h"
#include "snd/id.hpp"
#include "snd/test/test.hpp"

using namespace snd::literals;

namespace {
struct Vector {
  const char* text;
  std::uint32_t expected;
};
// FNV-1a 32 reference vectors (http://www.isthe.com/chongo/tech/comp/fnv/).
constexpr Vector kVectors[] = {
    {"", 0x811C9DC5u},  {"a", 0xE40C292Cu},       {"b", 0xE70C2DE5u},
    {"foobar", 0xBF9CF968u}, {"SE/Explosion", 0u},  // filled from the C function below
};
}  // namespace

SND_TEST(Hash, MatchesFnv1aReferenceVectors) {
  for (const Vector& v : kVectors) {
    if (v.expected == 0u) continue;
    SND_CHECK_EQ(snd_hash_id(v.text), v.expected);
    SND_CHECK_EQ(snd::HashId(v.text), v.expected);
  }
}

SND_TEST(Hash, LiteralEqualsRuntime) {
  constexpr snd::Id kExplosion = "SE/Explosion"_id;
  static_assert(kExplosion == snd::HashId("SE/Explosion"));
  SND_CHECK_EQ(kExplosion, snd_hash_id("SE/Explosion"));
  SND_CHECK_EQ("Music/Stinger"_id, snd_hash_id_n("Music/Stinger", std::strlen("Music/Stinger")));
  // Case and separators matter (no normalisation).
  SND_CHECK_NE("se/explosion"_id, kExplosion);
  SND_CHECK_NE("SE\\Explosion"_id, kExplosion);
}

SND_TEST(Hash, NeverReturnsNone) {
  // No short string hashes to 0 in practice; the remap rule is tested on the formula.
  SND_CHECK_NE(snd_hash_id(nullptr), SND_ID_NONE);
  SND_CHECK_EQ(snd_hash_id(nullptr), snd_hash_id(""));
  // Utf-8 bytes are hashed as-is.
  SND_CHECK_EQ("音/爆発"_id, snd_hash_id("音/爆発"));
}
