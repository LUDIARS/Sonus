#include "detail/crc32c.hpp"

#include <cstring>

namespace snd::bank::detail {
namespace {

struct Tables {
  std::uint32_t t[8][256];
};

constexpr Tables MakeTables() noexcept {
  Tables r{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
    r.t[0][i] = c;
  }
  for (std::uint32_t i = 0; i < 256; ++i) {
    for (int s = 1; s < 8; ++s) r.t[s][i] = (r.t[s - 1][i] >> 8) ^ r.t[0][r.t[s - 1][i] & 0xFFu];
  }
  return r;
}

constexpr Tables kTables = MakeTables();

}  // namespace

std::uint32_t Crc32cFast(std::uint32_t crc, const std::uint8_t* data, std::size_t n) noexcept {
  std::uint32_t c = ~crc;
  while (n >= 8) {
    std::uint32_t lo;
    std::uint32_t hi;
    std::memcpy(&lo, data, 4);  // little endian (bank-format.md §1)
    std::memcpy(&hi, data + 4, 4);
    lo ^= c;
    c = kTables.t[7][lo & 0xFFu] ^ kTables.t[6][(lo >> 8) & 0xFFu] ^ kTables.t[5][(lo >> 16) & 0xFFu] ^
        kTables.t[4][lo >> 24] ^ kTables.t[3][hi & 0xFFu] ^ kTables.t[2][(hi >> 8) & 0xFFu] ^
        kTables.t[1][(hi >> 16) & 0xFFu] ^ kTables.t[0][hi >> 24];
    data += 8;
    n -= 8;
  }
  while (n-- > 0) c = (c >> 8) ^ kTables.t[0][(c ^ *data++) & 0xFFu];
  return ~c;
}

}  // namespace snd::bank::detail
