#include "patch_decode.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace snd::source {
namespace {
bool Range(float x, float low, float high) noexcept { return x >= low && x <= high; }

bool ValidGraph(const PatchNode* nodes, unsigned count) noexcept {
  if (!nodes || !count || count > kPatchMaxNodes || nodes[count-1].op != PatchOp::Output) return false;
  unsigned used = 1u << (count-1);
  for (unsigned i = count; i-- > 0;) {
    const auto& n = nodes[i];
    if (!(used & (1u << i)) || (n.op == PatchOp::Output && i+1 != count)) return false;
    unsigned inputs = 0, args = 0;
    switch (n.op) {
      case PatchOp::Constant: args = 1; if (!Range(n.arg[0], -16, 16)) return false; break;
      case PatchOp::Oscillator:
        args = 2;
        if (n.waveform > 4 || !Range(n.arg[0], 0, 192000) || !Range(n.arg[1], 0, 1)) return false;
        break;
      case PatchOp::WhiteNoise: break;
      case PatchOp::Add: case PatchOp::Multiply: inputs = 2; break;
      case PatchOp::Lowpass: inputs = 1; args = 1; if (!Range(n.arg[0], 0, 192000)) return false; break;
      case PatchOp::Adsr:
        args = 4;
        if (!Range(n.arg[0], 0, 60) || !Range(n.arg[1], 0, 60) || !Range(n.arg[2], 0, 1) ||
            !Range(n.arg[3], 0, 60)) return false;
        break;
      case PatchOp::Lfo: case PatchOp::RandomHold: args = 1; if (!Range(n.arg[0], 0, 200)) return false; break;
      case PatchOp::Clip: inputs = 1; args = 1; if (!Range(n.arg[0], 0, 16)) return false; break;
      case PatchOp::Output: inputs = 1; break;
      default: return false;
    }
    if (n.op != PatchOp::Oscillator && n.waveform != 0) return false;
    if (inputs >= 1 ? n.a >= i : n.a != kPatchNoInput) return false;
    if (inputs == 2 ? n.b >= i : n.b != kPatchNoInput) return false;
    if (inputs >= 1) used |= 1u << n.a;
    if (inputs == 2) used |= 1u << n.b;
    for (unsigned j = args; j < 4; ++j) if (n.arg[j] != 0) return false;
  }
  return true;
}
unsigned U16(const std::uint8_t* p) noexcept { return p[0] | (unsigned(p[1]) << 8); }
}  // namespace

unsigned detail::DecodePatch(const std::uint8_t* data, std::size_t bytes, PatchNode* out) noexcept {
  if (!data || bytes < 12 || std::memcmp(data, "PTCH", 4) || U16(data+4) != 1 || U16(data+10) != 0) return 0;
  const unsigned count = U16(data+6);
  if (!count || count > kPatchMaxNodes || U16(data+8) != count-1 || bytes != PatchProgramBytes(count)) return 0;
  PatchNode nodes[kPatchMaxNodes];
  for (unsigned i = 0; i < count; ++i) {
    const auto* p = data + 12 + 20*i;
    auto& n = nodes[i];
    n.op = static_cast<PatchOp>(p[0]);
    n.a = p[1];
    n.b = p[2];
    n.waveform = p[3];
    for (unsigned j = 0; j < 4; ++j) {
      const auto* f = p + 4 + 4*j;
      std::uint32_t bits = 0;
      for (unsigned k = 0; k < 4; ++k) bits |= std::uint32_t{f[k]} << (8*k);
      n.arg[j] = std::bit_cast<float>(bits);
    }
  }
  if (!ValidGraph(nodes, count)) return 0;
  if (out) std::copy_n(nodes, count, out);
  return count;
}
bool ValidatePatchProgram(const std::uint8_t* data, std::size_t bytes) noexcept {
  return detail::DecodePatch(data, bytes, nullptr) != 0;
}
std::size_t BuildPatchProgram(const PatchNode* nodes, unsigned count, std::uint8_t* dst,
                              std::size_t capacity) noexcept {
  if (!dst || capacity < PatchProgramBytes(count) || !ValidGraph(nodes, count)) return 0;
  const std::uint8_t header[] = {'P','T','C','H',1,0,static_cast<std::uint8_t>(count),0,
                                 static_cast<std::uint8_t>(count-1),0,0,0};
  std::memcpy(dst, header, sizeof(header));
  for (unsigned i = 0; i < count; ++i) {
    auto* p = dst + 12 + 20*i;
    const auto& n = nodes[i];
    p[0] = static_cast<std::uint8_t>(n.op);
    p[1] = n.a;
    p[2] = n.b;
    p[3] = n.waveform;
    for (unsigned j = 0; j < 4; ++j) {
      const auto bits = std::bit_cast<std::uint32_t>(n.arg[j]);
      for (unsigned k = 0; k < 4; ++k) p[4 + 4*j+k] = static_cast<std::uint8_t>(bits >> (8*k));
    }
  }
  return PatchProgramBytes(count);
}
}  // namespace snd::source
