// Basic buffer kernels (design.md 3.4, 4 step 5-6).
//
// Conventions for the whole dsp module:
//  - Audio is float32, non-interleaved: one pointer per channel. Buffers handed
//    out by core are 64-byte aligned (snd::kBufferAlignment); every function
//    here also accepts unaligned pointers (sub-ranges for sample-accurate starts).
//  - Gain / parameter ramps follow ParamBlock (CONTRACTS.md D-26): `start` is
//    the value at frame 0, `end` the value just after the last frame, so frame i
//    of an n-frame block uses start + (end - start) * i / n and the next block
//    continues seamlessly from `end`.
//  - Nothing here allocates, locks, does IO or logs (principle 1); every
//    function may run on the audio thread and RT workers.
#ifndef SND_DSP_KERNELS_HPP
#define SND_DSP_KERNELS_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext/types.hpp"

namespace snd::dsp {

inline constexpr std::size_t kAlignment = 64;  // 3.4
inline constexpr std::uint32_t kMaxChannels = snd::kMaxChannels;

// dst[0..n) = 0
void Clear(float* dst, std::uint32_t frames) noexcept;
void Clear(float* const* channels, std::uint32_t channelCount, std::uint32_t frames) noexcept;
// dst[0..n) = src[0..n). Overlap is allowed only if dst == src.
void Copy(float* dst, const float* src, std::uint32_t frames) noexcept;
// dst += src
void Mix(float* dst, const float* src, std::uint32_t frames) noexcept;
// buf *= gain
void Scale(float* buf, std::uint32_t frames, float gain) noexcept;
// dst = src * g(i), g ramps from gainStart to gainEnd (see conventions). dst may equal src.
void CopyGainRamp(float* dst, const float* src, std::uint32_t frames, float gainStart,
                  float gainEnd) noexcept;
// dst += src * g(i). Constant gain when gainStart == gainEnd.
void MixGainRamp(float* dst, const float* src, std::uint32_t frames, float gainStart,
                 float gainEnd) noexcept;
// Multi-channel forms (same gain ramp on every channel).
void CopyGainRamp(float* const* dst, const float* const* src, std::uint32_t channels,
                  std::uint32_t frames, float gainStart, float gainEnd) noexcept;
void MixGainRamp(float* const* dst, const float* const* src, std::uint32_t channels,
                 std::uint32_t frames, float gainStart, float gainEnd) noexcept;

// max |x| (0 for an empty range). NaN inputs are ignored.
float Peak(const float* src, std::uint32_t frames) noexcept;
// Sum of x^2 in double precision accumulation order fixed per SIMD path.
double SumOfSquares(const float* src, std::uint32_t frames) noexcept;
// True if any sample is NaN or +-Inf.
bool HasNonFinite(const float* src, std::uint32_t frames) noexcept;
// True if any sample is a (non-zero) denormal.
bool HasDenormal(const float* src, std::uint32_t frames) noexcept;

// Interleaved <-> non-interleaved. out/in hold frames * channels samples.
void Interleave(const float* const* in, std::uint32_t channels, std::uint32_t frames,
                float* out) noexcept;
void Deinterleave(const float* in, std::uint32_t channels, std::uint32_t frames,
                  float* const* out) noexcept;

// dB <-> linear gain (amplitude). DbToGain(-inf) = 0; GainToDb(0) = -inf clamped to kMinDb.
inline constexpr float kMinDb = -144.0f;
float DbToGain(float db) noexcept;
float GainToDb(float gain) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_KERNELS_HPP
