// PTCH v1. All multibyte values are little endian; no alignment is required.
// BuildPatchProgram writes an entire validated program to caller-owned storage.
#ifndef SND_SOURCE_PATCH_PROGRAM_HPP
#define SND_SOURCE_PATCH_PROGRAM_HPP
#include <cstddef>
#include <cstdint>

namespace snd::source {
enum class PatchOp : std::uint8_t {
  Constant, Oscillator, WhiteNoise, Add, Multiply, Lowpass, Adsr, Lfo, RandomHold, Clip, Output
};
inline constexpr unsigned kPatchMaxNodes = 16;
inline constexpr std::uint8_t kPatchNoInput = 255;
inline constexpr std::size_t PatchProgramBytes(unsigned count) noexcept { return 12u + 20u * std::size_t{count}; }
struct PatchNode {
  PatchOp op = PatchOp::Constant;
  std::uint8_t a = kPatchNoInput, b = kPatchNoInput;
  std::uint8_t waveform = 0;  // oscillator: bank_format::Waveform
  float arg[4]{};             // see D-204, D-06-patch-format.md
};
bool ValidatePatchProgram(const std::uint8_t* data, std::size_t bytes) noexcept;
// Returns the byte count, or zero on error without changing dst.
std::size_t BuildPatchProgram(const PatchNode* nodes, unsigned count,
                              std::uint8_t* dst, std::size_t capacity) noexcept;
}  // namespace snd::source
#endif
