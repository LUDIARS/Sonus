// Generic parts moved into base (A1-12): CRC-32C, FNV-1a 64, LE access, AlignUp64.
#include <cstdint>
#include <vector>

#include "snd/base/atomic.hpp"
#include "snd/base/crc32c.hpp"
#include "snd/base/endian.hpp"
#include "snd/base/hash.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"

namespace b = snd::base;

SND_TEST(CommonParts, Crc32cMatchesTheContractReference) {
  // RFC 3720 test vector: CRC-32C("123456789") = 0xE3069283.
  const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  SND_CHECK_EQ(b::Crc32c(digits, 9), 0xE3069283u);
  std::vector<std::uint8_t> data(4099);
  std::uint32_t x = 12345u;
  for (auto& v : data) {
    x = x * 1664525u + 1013904223u;
    v = static_cast<std::uint8_t>(x >> 24);
  }
  for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{9},
                        std::size_t{63}, std::size_t{4099}}) {
    SND_CHECK_EQ(b::Crc32c(data.data(), n), snd::bank_format::Crc32c(data.data(), n));
  }
  // Split anywhere (and at unaligned starts) == whole.
  const std::uint32_t whole = b::Crc32c(data.data(), data.size());
  for (std::size_t cut : {std::size_t{1}, std::size_t{5}, std::size_t{8}, std::size_t{1000}, std::size_t{4098}}) {
    SND_CHECK_EQ(b::Crc32cUpdate(b::Crc32c(data.data(), cut), data.data() + cut, data.size() - cut), whole);
  }
}

SND_TEST(CommonParts, Fnv1a64) {
  // FNV-1a 64 of "" and "a" (reference values).
  SND_CHECK_EQ(b::Fnv1a64().value(), 14695981039346656037ull);
  SND_CHECK_EQ(b::Fnv1a64().Bytes("a", 1).value(), 0xAF63DC4C8601EC8Cull);
  std::uint8_t le[8];
  b::StoreLE64(le, 0x0102030405060708ull);
  SND_CHECK_EQ(b::Fnv1a64().U64(0x0102030405060708ull).value(), b::Fnv1a64().Bytes(le, 8).value());
  SND_CHECK(b::Fnv1a64().Str("ab").Str("c").value() != b::Fnv1a64().Str("a").Str("bc").value());
  static_assert(b::Fnv1a64().U64(1).value() != b::Fnv1a64().U64(2).value());
}

SND_TEST(CommonParts, LittleEndianAndAlign) {
  std::uint8_t p[8] = {};
  b::StoreLE16(p, 0xA1B2u);
  SND_CHECK_EQ(p[0], 0xB2u);
  SND_CHECK_EQ(b::LoadLE16(p), 0xA1B2u);
  b::StoreLE32(p, 0xA1B2C3D4u);
  SND_CHECK_EQ(p[0], 0xD4u);
  SND_CHECK_EQ(p[3], 0xA1u);
  SND_CHECK_EQ(b::LoadLE32(p), 0xA1B2C3D4u);
  SND_CHECK_EQ(b::LoadLE24(p), 0xB2C3D4u);
  b::StoreLE64(p, 0x1122334455667788ull);
  SND_CHECK_EQ(p[0], 0x88u);
  SND_CHECK_EQ(b::LoadLE64(p), 0x1122334455667788ull);
  static_assert(b::AlignUp64(0, 64) == 0 && b::AlignUp64(1, 64) == 64 && b::AlignUp64(64, 64) == 64);
  static_assert(b::AlignUp64((1ull << 40) + 1, 4096) == (1ull << 40) + 4096);
}
