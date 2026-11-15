// Voice resamplers (design.md 2.4, 4 step 5; CONTRACTS.md D-22).
//
// Core resamples source-rate kinds (Sample, Stream, UserBuffer, Input) from
// the source rate to the internal rate. The rate ratio is continuously
// variable and ramps linearly inside a block:
//
//   ratio = source frames advanced per output frame
//         = sourceRate / internalRate * pitch          (> 0)
//   output frame i of n uses ratio(i) = ratioStart + (ratioEnd - ratioStart) * i / n
//
// Positions are 32.32 fixed point, so a constant ratio gives bit-identical
// output however the frames are split into calls (block-split invariance).
//
// Qualities:
//   Sinc   - high quality: 32-tap Kaiser-windowed sinc, 128 polyphase rows with
//            linear interpolation between rows. The cutoff follows the ratio
//            (anti-aliasing when the ratio is > 1); tables are chosen per call
//            from max(ratioStart, ratioEnd). Passband (+-0.1 dB) to ~0.28-0.37 *
//            min(srcRate, outRate) for ratios <= 2; above 2 the fixed 32 taps narrow
//            it to ~(0.45 / ratio - 0.085) * srcRate. Stopband >= ~80 dB.
//   Cubic  - light: 4-point Hermite (Catmull-Rom). Used for NEW voices while the
//            load LOD asks for cheaper resampling (2.4); no anti-aliasing filter.
//   Linear - 2-point linear (cheapest).
//
// Calling sequence per voice and block (all RT safe, no allocation):
//   1. n = ResamplerInputFrames(state, outFrames, r0, r1)   // how many new source frames
//   2. the source writes n frames to in[c] + ResamplerPadFrames(q)   (zero-fill past the end)
//   3. ResamplerProcess(state, in, n, out, outFrames, r0, r1)
// `in[c]` must hold ResamplerPadFrames(q) + n floats. The first
// ResamplerPadFrames(q) floats are scratch the resampler overwrites with its
// history; with the pad a multiple of 16 frames, in[c] + pad stays 64-byte
// aligned when in[c] is. The input is not otherwise modified.
//
// The first output frame after Init/Reset is exactly the first input frame
// (no latency; the sinc reads 16 frames ahead, which is what InputFrames asks for).
//
// State: ResamplerStateBytes(q, channels) bytes, 16-byte aligned, owned by
// the caller (RealVoice.resamplerState when ResamplerFitsInline(), otherwise
// a side pool, see CONTRACTS proposal in the dsp report).
#ifndef SND_DSP_RESAMPLER_HPP
#define SND_DSP_RESAMPLER_HPP

#include <cstddef>
#include <cstdint>

#include "snd/internal/common.hpp"

namespace snd::dsp {

enum class ResamplerQuality : std::uint32_t {
  Linear = 0,
  Cubic = 1,
  Sinc = 2,
};
inline constexpr ResamplerQuality kResamplerHigh = ResamplerQuality::Sinc;
inline constexpr ResamplerQuality kResamplerLight = ResamplerQuality::Cubic;  // load LOD (2.4)

// Ratio limits; ratios outside are clamped.
inline constexpr float kResamplerMinRatio = 1.0f / 256.0f;
inline constexpr float kResamplerMaxRatio = 32.0f;

// Frames of history kept between calls.
std::uint32_t ResamplerHistoryFrames(ResamplerQuality q) noexcept;
// Scratch frames in front of the new input (>= history, multiple of 16).
std::uint32_t ResamplerPadFrames(ResamplerQuality q) noexcept;
// State bytes for `channels` (1..kMaxChannels).
std::size_t ResamplerStateBytes(ResamplerQuality q, std::uint32_t channels) noexcept;
// True if the state fits the 64-byte RealVoice.resamplerState column.
bool ResamplerFitsInline(ResamplerQuality q, std::uint32_t channels) noexcept;

// Initialises (silent history, phase 0).
void ResamplerInit(void* state, ResamplerQuality q, std::uint32_t channels) noexcept;
// Clears history and phase, keeps quality and channels (voice restart / seek).
void ResamplerReset(void* state) noexcept;
ResamplerQuality ResamplerGetQuality(const void* state) noexcept;
std::uint32_t ResamplerGetChannels(const void* state) noexcept;
// Fractional source position of the next output frame relative to the next
// unread input frame, in frames (for playhead bookkeeping; usually in (-1, 1]).
double ResamplerPendingOffset(const void* state) noexcept;

// Number of new input frames the next ResamplerProcess with the same
// arguments consumes. Pure.
std::uint32_t ResamplerInputFrames(const void* state, std::uint32_t outFrames, float ratioStart,
                                   float ratioEnd) noexcept;

// Writes outFrames frames to out[c] (overwrites). Returns the frames consumed
// (== ResamplerInputFrames). If inFrames is smaller than required, outputs
// silence, leaves the state unchanged and returns 0.
std::uint32_t ResamplerProcess(void* state, float* const* in, std::uint32_t inFrames,
                               float* const* out, std::uint32_t outFrames, float ratioStart,
                               float ratioEnd) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_RESAMPLER_HPP
