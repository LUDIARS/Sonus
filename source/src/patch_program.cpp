#include "patch_decode.hpp"

namespace snd::source {
unsigned detail::DecodePatch(const std::uint8_t* data, std::size_t bytes, PatchNode* out) noexcept {
  return bank_format::patch::Decode(data, bytes, out);
}
bool ValidatePatchProgram(const std::uint8_t* data, std::size_t bytes) noexcept {
  return bank_format::patch::ValidatePatchProgram(data, bytes);
}
std::size_t BuildPatchProgram(const PatchNode* nodes, unsigned count, std::uint8_t* dst,
                              std::size_t capacity) noexcept {
  return bank_format::patch::Encode(nodes, count, dst, capacity);
}
}  // namespace snd::source
