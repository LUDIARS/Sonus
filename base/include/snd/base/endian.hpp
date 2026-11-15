// Little-endian loads and stores of unaligned bytes (bank-format.md §1, codec
// byte layouts, WAV). Endian-independent: they assemble bytes explicitly.
// Moved here from codec/src/detail.hpp and buildtool's WAV reader (A1-12).
// All constexpr-friendly, RT-safe, any thread.
#ifndef SND_BASE_ENDIAN_HPP
#define SND_BASE_ENDIAN_HPP

#include <cstdint>

namespace snd::base {

inline std::uint16_t LoadLE16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t LoadLE24(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16);
}
inline std::uint32_t LoadLE32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t LoadLE64(const std::uint8_t* p) noexcept {
  return static_cast<std::uint64_t>(LoadLE32(p)) | (static_cast<std::uint64_t>(LoadLE32(p + 4)) << 32);
}
inline void StoreLE16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void StoreLE32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}
inline void StoreLE64(std::uint8_t* p, std::uint64_t v) noexcept {
  StoreLE32(p, static_cast<std::uint32_t>(v));
  StoreLE32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

}  // namespace snd::base

#endif  // SND_BASE_ENDIAN_HPP
