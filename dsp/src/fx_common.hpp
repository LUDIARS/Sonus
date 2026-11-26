// Internal framework shared by the standard effects (effects.cpp, fx_*.cpp).
#ifndef SND_DSP_SRC_FX_COMMON_HPP
#define SND_DSP_SRC_FX_COMMON_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "detail.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/kernels.hpp"

namespace snd::dsp::fx {

inline constexpr std::uint32_t kFxMagic = 0x31584653u;  // 'SFX1'

// First 128 bytes of every standard effect state.
struct FxHeader {
  std::uint32_t magic;
  std::uint32_t kind;          // EffectKind
  std::uint32_t channels;      // desc.channels
  std::uint32_t sampleRate;    // runtime rate (Init)
  std::uint32_t maxFrames;     // desc.max_frames
  std::uint32_t bytes;         // bytes available
  std::uint32_t silentFrames;  // consecutive frames with internal signal below kTailSilence
  std::uint32_t tailFrames;    // quiet frames needed before the tail is done
  std::uint32_t latency;       // frames
  std::uint32_t cleared;       // audio history cleared after the tail ended
  std::uint32_t paramCount;
  std::uint32_t sizingRate;    // rate the buffers were sized for (EffectReset re-runs Init)
  float cur[kMaxEffectParams];  // parameter values at the end of the last block
  std::uint32_t pad[4];
};
static_assert(sizeof(FxHeader) == 128);

struct FxConfig {
  std::uint32_t channels;
  std::uint32_t sizingRate;  // rate the buffers are sized for
  std::uint32_t sampleRate;  // runtime rate (<= sizingRate keeps full capacity)
  std::uint32_t maxFrames;
  float init[kMaxEffectParams];  // initial parameter values (clamped, defaults filled)
};

// Parameter values at block start and end (clamped).
struct FxParams {
  float s[kMaxEffectParams];
  float e[kMaxEffectParams];
  float At(std::uint32_t i, float t) const noexcept { return s[i] + (e[i] - s[i]) * t; }
  bool Ramps(std::uint32_t i) const noexcept { return s[i] != e[i]; }
};

struct FxIo {
  const float* const* in;
  float* const* out;
  std::uint32_t channels;
  std::uint32_t frames;
  const EffectSidechain* sidechain;
};

// Tail bookkeeping: `peak` = max |internal signal| written this block.
inline void TailUpdate(FxHeader& h, float peak, std::uint32_t frames) noexcept {
  if (peak < detail::kTailSilence) {
    const std::uint64_t s = static_cast<std::uint64_t>(h.silentFrames) + frames;
    h.silentFrames = s > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<std::uint32_t>(s);
  } else {
    h.silentFrames = 0;
    h.cleared = 0;
  }
}
// True once when the tail just ended and the history should be cleared.
inline bool TailShouldClear(FxHeader& h) noexcept {
  if (h.cleared == 0 && h.silentFrames >= h.tailFrames) {
    h.cleared = 1;
    return true;
  }
  return false;
}

inline float FramesToSeconds(std::uint32_t frames, std::uint32_t rate) noexcept {
  return static_cast<float>(frames) / static_cast<float>(rate);
}
inline std::uint32_t MsToFrames(float ms, std::uint32_t rate) noexcept {
  const double f = static_cast<double>(ms) * 0.001 * static_cast<double>(rate);
  return f <= 0.0 ? 0u : static_cast<std::uint32_t>(std::ceil(f));
}

// Per-kind entry points (fx_*.cpp). Layout: computes the bytes for cfg and,
// when state != nullptr, writes the offsets into the kind's state struct and
// zeroes the arrays. Init: derived values after Layout. Reset: clears history.
#define SND_FX_DECLARE(Name)                                                                 \
  std::size_t Name##Layout(void* state, const FxConfig& cfg) noexcept;                       \
  void Name##Init(void* state, const FxConfig& cfg) noexcept;                                \
  void Name##Process(void* state, const FxIo& io, const FxParams& p) noexcept;               \
  void Name##Reset(void* state) noexcept;

SND_FX_DECLARE(Reverb)
SND_FX_DECLARE(Echo)
SND_FX_DECLARE(Delay)
SND_FX_DECLARE(Chorus)
SND_FX_DECLARE(Flanger)
SND_FX_DECLARE(Distortion)
SND_FX_DECLARE(Compressor)
SND_FX_DECLARE(Limiter)
SND_FX_DECLARE(ParamEq)
SND_FX_DECLARE(Lowpass)
SND_FX_DECLARE(Highpass)
SND_FX_DECLARE(PitchShifter)
SND_FX_DECLARE(Normalize)
SND_FX_DECLARE(Surround)
SND_FX_DECLARE(Convolution)

#undef SND_FX_DECLARE

}  // namespace snd::dsp::fx

#endif  // SND_DSP_SRC_FX_COMMON_HPP
