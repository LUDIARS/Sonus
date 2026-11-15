// Independent tests of the generic parts moved into base (A1-12) against
// published test vectors and a bit-serial reference written here:
//  - CRC-32C (Castagnoli): RFC 3720 B.4 vectors; bit-serial reference over
//    random data at every start alignment 0..15 and lengths 0..300, continuation.
//  - FNV-1a 64: published vectors; Str = U64(length LE) + bytes; U64 is LE bytes.
//  - FNV-1a 32 ids (D-04): published vectors, IdHasher pieces == whole.
//  - LE loads / stores at unaligned addresses vs byte assembly.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "snd/base/crc32c.hpp"
#include "snd/base/endian.hpp"
#include "snd/base/hash.hpp"
#include "snd/test/test.hpp"

namespace b = snd::base;

namespace {
std::uint32_t BitSerialCrc32c(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
  }
  return ~crc;
}
}  // namespace

SND_TEST(IndepCommon11, Crc32cRfc3720Vectors) {
  std::uint8_t zeros[32] = {}, ones[32], inc[32], dec[32];
  for (int i = 0; i < 32; ++i) {
    ones[i] = 0xFF;
    inc[i] = static_cast<std::uint8_t>(i);
    dec[i] = static_cast<std::uint8_t>(31 - i);
  }
  SND_CHECK_EQ(b::Crc32c(zeros, 32), 0x8A9136AAu);
  SND_CHECK_EQ(b::Crc32c(ones, 32), 0x62A8AB43u);
  SND_CHECK_EQ(b::Crc32c(inc, 32), 0x46DD794Eu);
  SND_CHECK_EQ(b::Crc32c(dec, 32), 0x113FDB5Cu);
  // iSCSI SCSI Read (10) command PDU (RFC 3720 B.4).
  const std::uint8_t pdu[48] = {0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
                                0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  SND_CHECK_EQ(b::Crc32c(pdu, 48), 0xD9963A56u);
  SND_CHECK_EQ(b::Crc32c(nullptr, 0), 0u);
  const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  SND_CHECK_EQ(b::Crc32c(digits, 9), 0xE3069283u);
}

SND_TEST(IndepCommon11, Crc32cMatchesBitSerialAtEveryAlignment) {
  std::vector<std::uint8_t> data(400);
  std::uint64_t x = 0x9E3779B97F4A7C15ull;
  for (auto& v : data) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v = static_cast<std::uint8_t>(x);
  }
  std::size_t bad = 0;
  for (std::size_t start = 0; start < 16; ++start) {
    for (std::size_t n = 0; n <= 300; ++n) {
      if (b::Crc32c(data.data() + start, n) != BitSerialCrc32c(data.data() + start, n)) ++bad;
    }
  }
  SND_CHECK_EQ(bad, 0u);
  // Continuation from an arbitrary crc and split points.
  for (std::size_t cut = 0; cut <= 300; cut += 7) {
    const std::uint32_t a = b::Crc32cUpdate(0, data.data() + 3, cut);
    SND_CHECK_EQ(b::Crc32cUpdate(a, data.data() + 3 + cut, 300 - cut), BitSerialCrc32c(data.data() + 3, 300));
  }
  SND_CHECK_EQ(b::Crc32cUpdate(0xDEADBEEFu, data.data() + 1, 77), BitSerialCrc32c(data.data() + 1, 77, 0xDEADBEEFu));
}

SND_TEST(IndepCommon11, Fnv1a64Vectors) {
  SND_CHECK_EQ(b::Fnv1a64().value(), 0xCBF29CE484222325ull);
  SND_CHECK_EQ(b::Fnv1a64().Bytes("a", 1).value(), 0xAF63DC4C8601EC8Cull);
  SND_CHECK_EQ(b::Fnv1a64().Bytes("foobar", 6).value(), 0x85944171F73967E8ull);
  // Str = U64(length) then the bytes; U64 = 8 bytes little endian.
  const std::string s = "Main.sndb";
  std::uint8_t len[8];
  b::StoreLE64(len, s.size());
  SND_CHECK_EQ(b::Fnv1a64().Str(s).value(), b::Fnv1a64().Bytes(len, 8).Bytes(s.data(), s.size()).value());
  const std::uint8_t le[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  SND_CHECK_EQ(b::Fnv1a64().U64(0x0102030405060708ull).value(), b::Fnv1a64().Bytes(le, 8).value());
}

SND_TEST(IndepCommon11, Fnv1a32IdVectors) {
  // 32-bit FNV-1a (D-04): "" = offset basis, "a", "foobar".
  SND_CHECK_EQ(b::HashId(""), 0x811C9DC5u);
  SND_CHECK_EQ(b::HashId("a"), 0xE40C292Cu);
  SND_CHECK_EQ(b::HashId("foobar"), 0xBF9CF968u);
  SND_CHECK_EQ(b::HashIdCStr(nullptr), b::HashId(""));
  SND_CHECK_EQ(snd_hash_id("foobar"), 0xBF9CF968u);
  SND_CHECK_EQ(b::IdHasher().Append("SE/").Append("Explosion").Finish(), b::HashId("SE/Explosion"));
  SND_CHECK_EQ(b::IdHasher().Append("SE/Explosion", 12).Finish(), b::HashId("SE/Explosion"));
  // bank-format.md 6.4 ids are HashId of the names.
  SND_CHECK_EQ(b::HashId("snd.fx.reverb"), 0x542529D2u);
  SND_CHECK_EQ(b::HashId("snd.fx.surround"), 0x2072E34Cu);
}

SND_TEST(IndepCommon11, LittleEndianAtUnalignedAddresses) {
  std::uint8_t buf[32];
  for (std::size_t off = 0; off < 16; ++off) {
    std::memset(buf, 0xAA, sizeof(buf));
    b::StoreLE64(buf + off, 0x8877665544332211ull);
    for (int i = 0; i < 8; ++i) SND_CHECK_EQ(buf[off + i], static_cast<std::uint8_t>(0x11 * (i + 1)));
    SND_CHECK_EQ(buf[off + 8], 0xAAu);  // writes exactly 8 bytes
    SND_CHECK_EQ(b::LoadLE64(buf + off), 0x8877665544332211ull);
    SND_CHECK_EQ(b::LoadLE32(buf + off + 1), 0x55443322u);
    SND_CHECK_EQ(b::LoadLE24(buf + off + 2), 0x554433u);
    SND_CHECK_EQ(b::LoadLE16(buf + off + 6), 0x8877u);
    b::StoreLE32(buf + off, 0xFFFFFFFEu);
    SND_CHECK_EQ(b::LoadLE32(buf + off), 0xFFFFFFFEu);
    b::StoreLE16(buf + off, 0x8000u);
    SND_CHECK_EQ(static_cast<std::int16_t>(b::LoadLE16(buf + off)), -32768);
  }
}
