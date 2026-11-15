// Meters (design.md 4.6 "ラウドネス計測", 7 "解析"): peak, RMS, loudness and spectrum.
//
// Loudness method (decided in stage 1): ITU-R BS.1770-4 / EBU R128.
//   - K-weighting (high shelf +4 dB at ~1.68 kHz and RLB high-pass at ~38 Hz),
//     coefficients derived for any sample rate from the analogue prototypes.
//   - Channel weights: L, R, C 1.0; surrounds (SL, SR, BL, BR) 1.41; LFE excluded.
//     Layout inferred from the channel count (see pan.hpp).
//   - Energy is accumulated in 100 ms steps. Momentary = 400 ms window,
//     short-term = 3 s window (both sliding, updated every 100 ms).
//   - Integrated: 400 ms blocks with 75 % overlap, absolute gate -70 LUFS,
//     relative gate -10 LU. Blocks are kept in a histogram of 0.05 LU bins
//     from -70 to +10 LUFS (fixed memory, any duration); the relative gate is
//     evaluated at bin resolution.
//   - Values are LUFS; -inf (no signal) is reported as kLoudnessSilence.
// Spectrum: Hann-windowed FFT of the mono sum of the last fftSize frames;
// magnitudes are normalised so a full-scale sine centred on a bin reads 1.0.
#ifndef SND_DSP_METER_HPP
#define SND_DSP_METER_HPP

#include <cstddef>
#include <cstdint>

#include "snd/dsp/kernels.hpp"

namespace snd::dsp {

// ---------------------------------------------------------------------------
// Peak with release (linear amplitude).
// ---------------------------------------------------------------------------
struct PeakMeterState {
  float peak[kMaxChannels];
  float releasePerFrame;  // multiplicative decay per frame
  std::uint32_t channels;
};
// releaseDbPerSecond > 0 (e.g. 20 dB/s); 0 = hold forever.
void PeakMeterInit(PeakMeterState& s, std::uint32_t channels, float releaseDbPerSecond,
                   std::uint32_t sampleRate) noexcept;
void PeakMeterProcess(PeakMeterState& s, const float* const* in, std::uint32_t frames) noexcept;

// ---------------------------------------------------------------------------
// RMS with exponential averaging (time constant windowSeconds).
// ---------------------------------------------------------------------------
struct RmsMeterState {
  double meanSquare[kMaxChannels];
  double coef;  // per-frame decay
  std::uint32_t channels;
  std::uint32_t reserved;
};
void RmsMeterInit(RmsMeterState& s, std::uint32_t channels, float windowSeconds,
                  std::uint32_t sampleRate) noexcept;
void RmsMeterProcess(RmsMeterState& s, const float* const* in, std::uint32_t frames) noexcept;
float RmsMeterValue(const RmsMeterState& s, std::uint32_t channel) noexcept;

// ---------------------------------------------------------------------------
// Loudness (BS.1770-4 / R128). Fixed-size state (no allocation).
// ---------------------------------------------------------------------------
inline constexpr float kLoudnessSilence = -200.0f;
inline constexpr std::uint32_t kLoudnessHistogramBins = 1600;  // -70 .. +10 LUFS, 0.05 LU

struct LoudnessMeterState {
  // K-weighting biquads (2 stages), TDF-II state per channel.
  float shelf[5], highpass[5];
  float z[kMaxChannels][4];
  float weight[kMaxChannels];
  std::uint32_t channels;
  std::uint32_t sampleRate;
  std::uint32_t stepFrames;    // frames per 100 ms step
  std::uint32_t stepFill;      // frames accumulated in the current step
  double stepEnergy;           // weighted energy of the current step
  double steps[30];            // ring of the last 30 step energies (3 s), each a mean
  std::uint32_t stepWrite;     // next ring position
  std::uint32_t stepCount;     // steps seen (saturates)
  double momentaryEnergy;      // last 400 ms mean (weighted)
  double shortTermEnergy;      // last 3 s mean
  std::uint64_t gatedBlocks;   // blocks above the absolute gate
  std::uint32_t histCount[kLoudnessHistogramBins];
  double histEnergy[kLoudnessHistogramBins];
};
void LoudnessMeterInit(LoudnessMeterState& s, std::uint32_t channels,
                       std::uint32_t sampleRate) noexcept;
void LoudnessMeterReset(LoudnessMeterState& s) noexcept;  // keeps channels/rate
void LoudnessMeterProcess(LoudnessMeterState& s, const float* const* in,
                          std::uint32_t frames) noexcept;
float LoudnessMomentary(const LoudnessMeterState& s) noexcept;   // LUFS
float LoudnessShortTerm(const LoudnessMeterState& s) noexcept;   // LUFS
float LoudnessIntegrated(const LoudnessMeterState& s) noexcept;  // LUFS

// ---------------------------------------------------------------------------
// Spectrum (FFT). State in caller memory: SpectrumStateBytes(fftSize) bytes,
// 64-byte aligned. fftSize: power of two, 32..8192. Init is not RT (computes
// tables, but does not allocate); Push/Compute are RT safe.
// ---------------------------------------------------------------------------
std::size_t SpectrumStateBytes(std::uint32_t fftSize) noexcept;
bool SpectrumInit(void* state, std::uint32_t fftSize) noexcept;
// Appends the mono sum (mean of the channels) of `frames` frames.
void SpectrumPush(void* state, const float* const* in, std::uint32_t channels,
                  std::uint32_t frames) noexcept;
// Writes fftSize / 2 + 1 linear magnitudes (DC .. Nyquist).
void SpectrumCompute(void* state, float* magnitudes) noexcept;
std::uint32_t SpectrumSize(const void* state) noexcept;

// In-place complex FFT (radix 2) on interleaved (re, im) pairs; `n` power of 2.
// twiddles: n/2 complex values exp(-2 pi i k / n) (FftTwiddles). Exposed for tools/tests.
void FftTwiddles(float* twiddles, std::uint32_t n) noexcept;
void FftComplex(float* data, const float* twiddles, std::uint32_t n) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_METER_HPP
