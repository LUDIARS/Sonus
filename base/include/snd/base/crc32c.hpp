// CRC-32C (Castagnoli), table-driven slicing-by-8 (moved from bank, A1-12).
//
// Bit-identical to bank_format::Crc32cUpdate (the contract's reference
// implementation): Crc32cUpdate(Crc32cUpdate(0, a), b) == Crc32c(a ++ b).
// Data is read byte-wise as little endian (bank-format.md §1). RT-safe, any
// thread, no allocation.
#ifndef SND_BASE_CRC32C_HPP
#define SND_BASE_CRC32C_HPP

#include <cstddef>
#include <cstdint>

namespace snd::base {

// Continues `crc` (0 to start) over n bytes.
std::uint32_t Crc32cUpdate(std::uint32_t crc, const std::uint8_t* data, std::size_t n) noexcept;
inline std::uint32_t Crc32c(const std::uint8_t* data, std::size_t n) noexcept { return Crc32cUpdate(0u, data, n); }

}  // namespace snd::base

#endif  // SND_BASE_CRC32C_HPP
