// Real FFT of power-of-two size (design.md 3.4, 7 "エフェクト"; CONTRACTS.md D-208).
//
// Used by the partitioned convolution (convolution.hpp). Own implementation, no
// third-party code: complex radix-2 Stockham FFT of size n / 2 on split arrays
// (real and imaginary parts in separate arrays, SIMD over the contiguous inner
// index from the fourth stage on), wrapped by the usual even / odd packing for
// real input.
//
// Spectrum layout ("packed split"): for a real signal x[0..n), m = n / 2,
//   re[0] = X[0]   (DC, real)
//   im[0] = X[m]   (Nyquist, real)
//   re[k] + i im[k] = X[k] for 1 <= k < m,   X[k] = sum_j x[j] exp(-2 pi i j k / n).
// The bins above m are the conjugates and are not stored.
//
// Scaling: the forward transform is unnormalised; FftRealInverse(FftRealForward(x))
// returns n * x. Callers fold 1 / n into one side (the convolution scales the
// prepared impulse response).
//
// Numerics: float32 throughout, twiddles computed in double and rounded once. The
// butterflies use separate multiplies and adds (never fused), so the scalar and
// SIMD paths give the same bits; the round-trip error is about 1e-7 * log2(n)
// relative to the signal RMS (measured in dsp/test/fft_test.cpp).
//
// Real time: every function after FftPlanInit is RT safe (no allocation, no
// locks, no IO). The plan is caller memory (any alignment of 16 or more; the
// convolution keeps it in its DSP pool state) and is read only after Init, so
// one plan may be used by several threads at once. `work` is caller scratch of
// FftWorkFloats(n) floats, one per concurrent call.
#ifndef SND_DSP_FFT_HPP
#define SND_DSP_FFT_HPP

#include <cstddef>
#include <cstdint>

namespace snd::dsp {

inline constexpr std::uint32_t kFftMinSize = 4;
inline constexpr std::uint32_t kFftMaxSize = 1u << 17;  // 131072

// True if n is a power of two in [kFftMinSize, kFftMaxSize].
bool FftSizeValid(std::uint32_t n) noexcept;
// Plan bytes for size n (multiple of 64), 0 if n is not valid.
std::size_t FftPlanBytes(std::uint32_t n) noexcept;
// Writes the plan (twiddle tables). Not RT (trigonometry); no allocation.
// False if n is not valid or bytes < FftPlanBytes(n).
bool FftPlanInit(void* plan, std::size_t bytes, std::uint32_t n) noexcept;
// Size the plan was made for (0 if `plan` is not an initialised plan).
std::uint32_t FftPlanSize(const void* plan) noexcept;
// Scratch floats one call needs (= n).
inline constexpr std::size_t FftWorkFloats(std::uint32_t n) noexcept { return n; }

// x[0..n) -> packed split spectrum re[0..m), im[0..m). `in` is not modified and
// may not alias re / im / work. re, im and work must not overlap.
void FftRealForward(const void* plan, const float* in, float* re, float* im, float* work) noexcept;
// Packed split spectrum -> n * x[0..n). re / im are not modified; out must not
// overlap re, im or work.
void FftRealInverse(const void* plan, const float* re, const float* im, float* out,
                    float* work) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_FFT_HPP
