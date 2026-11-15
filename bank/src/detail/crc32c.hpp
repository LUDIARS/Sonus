// Table-driven CRC-32C (slicing-by-8), bit-identical to bank_format::Crc32c.
// detail: a generic part that belongs in base once base exists.
#ifndef SND_BANK_SRC_DETAIL_CRC32C_HPP
#define SND_BANK_SRC_DETAIL_CRC32C_HPP

#include <cstddef>
#include <cstdint>

namespace snd::bank::detail {

std::uint32_t Crc32cFast(std::uint32_t crc, const std::uint8_t* data, std::size_t n) noexcept;

}  // namespace snd::bank::detail

#endif  // SND_BANK_SRC_DETAIL_CRC32C_HPP
