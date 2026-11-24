#ifndef SND_SOURCE_PATCH_DECODE_HPP
#define SND_SOURCE_PATCH_DECODE_HPP
#include "snd/source/patch_program.hpp"
namespace snd::source::detail {
// On success copies validated nodes to out (if provided) and returns the count.
unsigned DecodePatch(const std::uint8_t*, std::size_t, PatchNode* out) noexcept;
}
#endif
