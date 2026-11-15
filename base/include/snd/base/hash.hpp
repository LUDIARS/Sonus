// Run-time ID hashing (design.md 10.1, CONTRACTS.md D-04).
//
// Bit-identical to snd_hash_id() / snd::HashId / "..."_id: 32-bit FNV-1a over
// the UTF-8 bytes, 0 remapped to 1. IdHasher hashes a name given in pieces
// (e.g. "SE/" + name) without building the string. All RT-safe, any thread.
#ifndef SND_BASE_HASH_HPP
#define SND_BASE_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "snd/id.h"
#include "snd/id.hpp"

namespace snd::base {

constexpr Id HashId(std::string_view name) noexcept { return snd::HashId(name); }
constexpr Id HashId(const char* data, std::size_t len) noexcept { return snd::HashId(std::string_view(data, len)); }
// NUL-terminated; nullptr hashes like "" (same as snd_hash_id).
constexpr Id HashIdCStr(const char* name) noexcept {
  return name == nullptr ? snd::HashId(std::string_view()) : snd::HashId(std::string_view(name));
}

class IdHasher {
 public:
  constexpr IdHasher& Append(std::string_view bytes) noexcept {
    for (char c : bytes) {
      h_ ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
      h_ *= SND_FNV1A32_PRIME;
    }
    return *this;
  }
  constexpr IdHasher& Append(const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
      h_ ^= static_cast<std::uint32_t>(p[i]);
      h_ *= SND_FNV1A32_PRIME;
    }
    return *this;
  }
  // The ID of everything appended so far (never kIdNone).
  constexpr Id Finish() const noexcept { return h_ == 0u ? 1u : h_; }
  // Raw FNV-1a state (no 0 remap), e.g. for hash tables.
  constexpr std::uint32_t Raw() const noexcept { return h_; }

 private:
  std::uint32_t h_ = SND_FNV1A32_OFFSET;
};

static_assert(IdHasher().Append("foo").Append("bar").Finish() == snd::HashId("foobar"));

// 32-bit integer mixer (for open-addressing tables keyed by Id or slot).
constexpr std::uint32_t MixU32(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

}  // namespace snd::base

#endif  // SND_BASE_HASH_HPP
