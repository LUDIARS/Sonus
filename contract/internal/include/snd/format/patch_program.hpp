// PTCH v1 (D-06). Shared by the bank checker, build tool and resident source.
#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iterator>

// Preserve the source helper API (including its C++ type names).
namespace snd::source {
enum class PatchOp : std::uint8_t {
  Constant,
  Oscillator,
  WhiteNoise,
  Add,
  Multiply,
  Lowpass,
  Adsr,
  Lfo,
  RandomHold,
  Clip,
  Output
};
inline constexpr unsigned kPatchMaxNodes = 16;
inline constexpr std::uint8_t kPatchNoInput = 255;
inline constexpr std::size_t PatchProgramBytes(unsigned count) noexcept {
  return 12u + 20u * std::size_t{count};
}
struct PatchNode {
  PatchOp op = PatchOp::Constant;
  std::uint8_t a = kPatchNoInput, b = kPatchNoInput;
  std::uint8_t waveform = 0;  // oscillator: bank_format::Waveform
  float arg[4]{};             // see D-204, D-06-patch-format.md
};
}  // namespace snd::source

namespace snd::bank_format::patch {
using snd::source::PatchOp;
using snd::source::PatchNode;
using snd::source::kPatchMaxNodes;
using snd::source::kPatchNoInput;
using snd::source::PatchProgramBytes;
struct Bounds { double lo, hi; };
struct OpInfo {
  const char* name;
  unsigned inputs, args;
  Bounds bounds[4];
};
// Index is the wire opcode. Unused arguments must contain zero.
inline constexpr OpInfo kOps[] = {
    {"constant", 0, 1, {{-16, 16}}}, {"oscillator", 0, 2, {{0, 192000}, {0, 1}}},
    {"whiteNoise", 0, 0, {}}, {"add", 2, 0, {}}, {"multiply", 2, 0, {}},
    {"lowpass", 1, 1, {{0, 192000}}}, {"adsr", 0, 4, {{0, 60}, {0, 60}, {0, 1}, {0, 60}}},
    {"lfo", 0, 1, {{0, 200}}}, {"randomHold", 0, 1, {{0, 200}}}, {"clip", 1, 1, {{0, 16}}},
    {"output", 1, 0, {}}};
static_assert(std::size(kOps) == static_cast<unsigned>(PatchOp::Output) + 1);

inline bool ValidGraph(const PatchNode* nodes, unsigned count) noexcept {
  if (!nodes || count < 2 || count > kPatchMaxNodes || nodes[count - 1].op != PatchOp::Output)
    return false;
  unsigned used = 1u << (count - 1);
  for (unsigned i = count; i-- > 0;) {
    const auto& n = nodes[i];
    if (!(used & (1u << i)) || (n.op == PatchOp::Output && i + 1 != count)) return false;
    const auto op = static_cast<unsigned>(n.op);
    if (op >= std::size(kOps) || n.waveform > 4) return false;
    const auto& info = kOps[op];
    const auto inputs = info.inputs;
    for (unsigned j = 0; j < 4; ++j) {
      if (!(n.arg[j] >= info.bounds[j].lo && n.arg[j] <= info.bounds[j].hi)) return false;
    }
    if (n.op != PatchOp::Oscillator && n.waveform != 0) return false;
    if (inputs >= 1 ? n.a >= i : n.a != kPatchNoInput) return false;
    if (inputs == 2 ? n.b >= i : n.b != kPatchNoInput) return false;
    if (inputs >= 1) used |= 1u << n.a;
    if (inputs == 2) used |= 1u << n.b;
  }
  return true;
}
inline unsigned U16(const std::uint8_t* p) noexcept {
  return p[0] | (unsigned(p[1]) << 8);
}

inline unsigned Decode(const std::uint8_t* data, std::size_t bytes, PatchNode* out) noexcept {
  if (!data || bytes < 12 || std::memcmp(data, "PTCH", 4) || U16(data + 4) != 1 ||
      U16(data + 10) != 0)
    return 0;
  const unsigned count = U16(data + 6);
  if (!count || count > kPatchMaxNodes || U16(data + 8) != count - 1 ||
      bytes != PatchProgramBytes(count))
    return 0;
  PatchNode nodes[kPatchMaxNodes];
  for (unsigned i = 0; i < count; ++i) {
    const auto* p = data + 12 + 20 * i;
    auto& n = nodes[i];
    n.op = static_cast<PatchOp>(p[0]);
    n.a = p[1];
    n.b = p[2];
    n.waveform = p[3];
    for (unsigned j = 0; j < 4; ++j) {
      const auto* f = p + 4 + 4 * j;
      std::uint32_t bits = 0;
      for (unsigned k = 0; k < 4; ++k) bits |= std::uint32_t{f[k]} << (8 * k);
      n.arg[j] = std::bit_cast<float>(bits);
    }
  }
  if (!ValidGraph(nodes, count)) return 0;
  if (out) std::copy_n(nodes, count, out);
  return count;
}
inline bool ValidatePatchProgram(const std::uint8_t* data, std::size_t bytes) noexcept {
  return Decode(data, bytes, nullptr) != 0;
}
inline std::size_t Encode(const PatchNode* nodes, unsigned count, std::uint8_t* dst,
                              std::size_t capacity) noexcept {
  if (!dst || capacity < PatchProgramBytes(count) || !ValidGraph(nodes, count)) return 0;
  const std::uint8_t header[] = {'P',
                                 'T',
                                 'C',
                                 'H',
                                 1,
                                 0,
                                 static_cast<std::uint8_t>(count),
                                 0,
                                 static_cast<std::uint8_t>(count - 1),
                                 0,
                                 0,
                                 0};
  std::memcpy(dst, header, sizeof(header));
  for (unsigned i = 0; i < count; ++i) {
    auto* p = dst + 12 + 20 * i;
    const auto& n = nodes[i];
    p[0] = static_cast<std::uint8_t>(n.op);
    p[1] = n.a;
    p[2] = n.b;
    p[3] = n.waveform;
    for (unsigned j = 0; j < 4; ++j) {
      const auto bits = std::bit_cast<std::uint32_t>(n.arg[j]);
      for (unsigned k = 0; k < 4; ++k)
        p[4 + 4 * j + k] = static_cast<std::uint8_t>(bits >> (8 * k));
    }
  }
  return PatchProgramBytes(count);
}
}  // namespace snd::bank_format::patch
