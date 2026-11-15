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
// A RAMPED ratio split into calls at the matching intermediate ratio gives the
// same positions up to the rounding of the per-frame increment (<= 1 frame of
// 2^-32 per call); for Sinc the filter is chosen per call from the call's
// largest ratio, so outputs may differ by up to ~1e-2 (measured 9e-3 for a
// 0.8 -> 1.6 ramp split in two) in the first ~16 frames after the split and by
// <= 1e-3 afterwards.
//
// Qualities:
//   Sinc   - high quality, chosen per call from r = max(ratioStart, ratioEnd):
//            r <= 4: 32-tap Kaiser-windowed sinc (beta 8), 128 polyphase rows with
//                    linear interpolation between rows, cutoff 0.45 * srcRate / r'
//                    (r' = r rounded up to 1, 1.1, 1.2, 1.35, 1.5, 1.7, 2, 2.4, 2.8, 3.4, 4).
//            r > 4:  ratio-scaled kernel, 2 * ceil(6 r) taps, transition centred on
//                    the output Nyquist (cutoff, transition and stopband fixed in
//                    OUTPUT frequency).
//            Measured (48 kHz output, Release, x86-64; dsp/test resampler tests):
//              passband (+-0.1 dB): r <= 1: to ~0.39 * srcRate; 1 < r <= 2: to
//                0.33..0.36 * outRate; r = 3: 0.215 * outRate; r = 4: 0.21 * outRate
//                (10 kHz); r > 4: to 0.33 * outRate (16 kHz).
//              aliasing / imaging: <= -79 dB for r <= 4 for tones in the table's
//                stopband (from (0.45 / r' + 0.08) * srcRate; at r = 4 a source tone
//                at 0.75 * outRate still aliases at -62 dB); <= -86 dB for r > 4 for
//                source tones from 0.75 * outRate up. Tones between the passband and
//                stopband edges fold to just below the output Nyquist, partially
//                attenuated.
//            Cost per voice and 256 output frames (mono / stereo): r <= 4: ~1.4 / 1.9 us;
//              r > 4 (per-call polyphase table of the scaled kernel, 32 KiB of stack):
//              4.5: 14 / 15 us, 8: 20 / 22 us, 16: 38 / 41 us, 32: 50 / 50 us.
//   Cubic  - light: 4-point Hermite (Catmull-Rom). Used for NEW voices while the
//            load LOD asks for cheaper resampling (2.4); no anti-aliasing filter.
//   Linear - 2-point linear (cheapest).
//
// Calling sequence per voice and block (all RT safe, no allocation):
//   1. n = ResamplerInputFrames(state, outFrames, r0, r1)   // how many new source frames
//   2. the source writes n frames to in[c] + ResamplerPadFrames(q, maxRatio)  (zero-fill past the end)
//   3. ResamplerProcess(state, in, n, out, outFrames, r0, r1)
// `in[c]` must hold ResamplerPadFrames(q, maxRatio) + n floats. The pad is
// scratch the resampler overwrites with its history; it is a multiple of 16
// frames, so in[c] + pad stays 64-byte aligned when in[c] is. The input is not
// otherwise modified.
//
// The first output frame after Init/Reset is exactly the first input frame
// (no latency; the resampler reads ResamplerLookaheadFrames(q, r) frames ahead,
// which is what InputFrames asks for).
//
// maxRatio (Init): ratios are clamped to [kResamplerMinRatio, maxRatio]. It sets
// the history kept (state size and pad): Sinc keeps 2 * lookahead(maxRatio)
// frames = 32 for maxRatio <= 4 and 2 * ceil(6 maxRatio) above (384 at 32).
// Core may pass a smaller maxRatio (e.g. 4 for voices whose pitch range is
// known) to keep the state small.
//
// State: ResamplerStateBytes(q, channels, maxRatio) bytes, 16-byte aligned,
// owned by the caller (RealVoice.resamplerState when ResamplerFitsInline(),
// otherwise a side pool, see the contract proposal in the dsp report).
// Init writes every byte; the memory may hold anything before.
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

// Frames of history kept between calls (Linear 2, Cubic 4, Sinc see above).
std::uint32_t ResamplerHistoryFrames(ResamplerQuality q,
                                     float maxRatio = kResamplerMaxRatio) noexcept;
// Frames read ahead of the current position at `ratio` (Linear 1, Cubic 2,
// Sinc 16 up to ratio 4, ceil(6 ratio) above).
std::uint32_t ResamplerLookaheadFrames(ResamplerQuality q, float ratio) noexcept;
// Scratch frames in front of the new input (>= history, multiple of 16).
std::uint32_t ResamplerPadFrames(ResamplerQuality q, float maxRatio = kResamplerMaxRatio) noexcept;
// State bytes for `channels` (1..kMaxChannels).
std::size_t ResamplerStateBytes(ResamplerQuality q, std::uint32_t channels,
                                float maxRatio = kResamplerMaxRatio) noexcept;
// True if the state fits the 64-byte RealVoice.resamplerState column
// (Linear <= 6 channels, Cubic <= 3, Sinc never).
bool ResamplerFitsInline(ResamplerQuality q, std::uint32_t channels,
                         float maxRatio = kResamplerMaxRatio) noexcept;

// Initialises (silent history, phase 0).
void ResamplerInit(void* state, ResamplerQuality q, std::uint32_t channels,
                   float maxRatio = kResamplerMaxRatio) noexcept;
// Same as a fresh Init with the same quality, channels and maxRatio.
void ResamplerReset(void* state) noexcept;
ResamplerQuality ResamplerGetQuality(const void* state) noexcept;
std::uint32_t ResamplerGetChannels(const void* state) noexcept;
// Source position of the next output frame relative to the next unread input
// frame, in frames. 0 after Init/Reset. After a call whose last output frame
// used ratio r and look-ahead R (ResamplerLookaheadFrames):
//   offset in [r - R - 1, r - R)        (unless the call consumed no input,
//                                         then it grows by the call's advance)
// i.e. Linear [r - 2, r - 1), Cubic [r - 3, r - 2), Sinc (r <= 4) [r - 17, r - 16).
// The playhead of the voice is (frames consumed + offset).
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
