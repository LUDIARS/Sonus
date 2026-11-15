// Filters: the per-voice lowpass/highpass every real voice has (design.md 7
// "再生単位のフィルタ"), and biquads (RBJ cookbook) with coefficient
// interpolation for effects and tools.
#ifndef SND_DSP_FILTER_HPP
#define SND_DSP_FILTER_HPP

#include <cstdint>

#include "snd/dsp/kernels.hpp"
#include "snd/internal/common.hpp"

namespace snd::dsp {

// ---------------------------------------------------------------------------
// Voice filter: 12 dB/oct lowpass followed by 12 dB/oct highpass (TPT state
// variable filters, Butterworth Q). State = RealVoice.filterState (256 bytes).
//
// FilterTarget (Voice.filterTarget) semantics:
//   lowpassHz  >= kVoiceLowpassOffHz  (or >= 0.45 * sampleRate): lowpass off
//   highpassHz <= kVoiceHighpassOffHz: highpass off
// Active cutoffs are clamped to [10 Hz, 0.45 * sampleRate]. Coefficients are
// interpolated every 16 frames across the block (linearly in the prewarped
// gain tan(pi f / fs), which is close to geometric in frequency).
// Switching a filter on starts from the steady state of the current input
// (no click on DC/low frequencies); switching off ramps to the open limit
// first. Processing is in place.
// ---------------------------------------------------------------------------
inline constexpr float kVoiceLowpassOffHz = 20000.0f;
inline constexpr float kVoiceHighpassOffHz = 10.0f;
inline constexpr FilterTarget kVoiceFilterOpen{1.0e9f, 0.0f};

// Remembers `initial` as the current cutoffs (no sweep on the first block).
void VoiceFilterInit(FilterState& state, const FilterTarget& initial) noexcept;
// Ramps from the cutoffs used at the end of the previous call to `target`.
void VoiceFilterProcess(FilterState& state, float* const* buffers, std::uint32_t channels,
                        std::uint32_t frames, const FilterTarget& target,
                        std::uint32_t sampleRate) noexcept;
// Explicit ramp (start -> end) variant; the state remembers `end`.
void VoiceFilterProcessRamp(FilterState& state, float* const* buffers, std::uint32_t channels,
                            std::uint32_t frames, const FilterTarget& start,
                            const FilterTarget& end, std::uint32_t sampleRate) noexcept;
// True if both filters are off at the current cutoffs (core may skip the call
// while the target also stays off).
bool VoiceFilterIsOpen(const FilterState& state, std::uint32_t sampleRate) noexcept;

// ---------------------------------------------------------------------------
// Biquads (Robert Bristow-Johnson's cookbook), transposed direct form II.
// ---------------------------------------------------------------------------
enum class BiquadType : std::uint32_t {
  Lowpass = 0,
  Highpass = 1,
  Bandpass = 2,  // constant 0 dB peak gain
  Notch = 3,
  Peaking = 4,
  LowShelf = 5,
  HighShelf = 6,
  Allpass = 7,
};

struct BiquadCoefs {
  float b0, b1, b2, a1, a2;  // a0 normalised to 1
};
inline constexpr BiquadCoefs kBiquadIdentity{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// freqHz is clamped to [1 Hz, 0.49 * sampleRate], q to [0.05, 100].
// gainDb is used by Peaking / LowShelf / HighShelf (shelf slope S = 1 when q <= 0).
BiquadCoefs DesignBiquad(BiquadType type, float freqHz, float q, float gainDb,
                         float sampleRate) noexcept;
// |H(e^jw)| at freqHz (double precision; tests and tools).
double BiquadMagnitude(const BiquadCoefs& c, double freqHz, double sampleRate) noexcept;
// Converts an octave bandwidth (Unity ParamEQ "octave range") to Q.
float OctaveBandwidthToQ(float octaves) noexcept;

struct BiquadState {
  float z1[kMaxChannels];
  float z2[kMaxChannels];
};
void BiquadReset(BiquadState& s) noexcept;
// Coefficients interpolate linearly per frame from `from` to `to` (the
// (a1, a2) stability triangle is convex, so the path stays stable).
// in and out may alias.
void BiquadProcess(BiquadState& s, const BiquadCoefs& from, const BiquadCoefs& to,
                   const float* const* in, float* const* out, std::uint32_t channels,
                   std::uint32_t frames) noexcept;
// Flushes tiny state values to zero (no denormals without FTZ/DAZ).
void BiquadFlush(BiquadState& s, std::uint32_t channels) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_FILTER_HPP
