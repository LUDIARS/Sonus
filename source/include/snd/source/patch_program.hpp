// PTCH v1. All multibyte values are little endian; no alignment is required.
// BuildPatchProgram writes an entire validated program to caller-owned storage.
#ifndef SND_SOURCE_PATCH_PROGRAM_HPP
#define SND_SOURCE_PATCH_PROGRAM_HPP
#include "snd/format/patch_program.hpp"
#include <cstddef>
#include <cstdint>

namespace snd::source {
bool ValidatePatchProgram(const std::uint8_t* data, std::size_t bytes) noexcept;
// Returns the byte count, or zero on error without changing dst.
std::size_t BuildPatchProgram(const PatchNode* nodes, unsigned count, std::uint8_t* dst,
                              std::size_t capacity) noexcept;
}  // namespace snd::source
#endif
