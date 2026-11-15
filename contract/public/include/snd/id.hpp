// Compile-time and run-time IDs (design.md 10.1). FROZEN CONTRACT.
//
// snd::HashId and the "..."_id literal produce exactly snd_hash_id() of
// snd/id.h (FNV-1a 32, 0 remapped to 1). buildtool uses the same function.
//
//   using namespace snd::literals;      // or: using namespace snd;
//   snd::Play(sys, "SE/Explosion"_id, emitter);
#ifndef SND_ID_HPP
#define SND_ID_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "snd/id.h"

namespace snd {

using Id = snd_id;
inline constexpr Id kIdNone = SND_ID_NONE;

constexpr Id HashId(std::string_view name) noexcept {
  std::uint32_t h = SND_FNV1A32_OFFSET;
  for (char c : name) {
    h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    h *= SND_FNV1A32_PRIME;
  }
  return h == 0u ? 1u : h;
}

inline namespace literals {
consteval Id operator""_id(const char* s, std::size_t n) { return HashId(std::string_view(s, n)); }
}  // namespace literals

static_assert(HashId("") == 2166136261u, "FNV-1a 32 offset basis");
static_assert(HashId("a") == 0xE40C292Cu, "FNV-1a 32 test vector");
static_assert(HashId("foobar") == 0xBF9CF968u, "FNV-1a 32 test vector");

}  // namespace snd

#endif  // SND_ID_HPP
