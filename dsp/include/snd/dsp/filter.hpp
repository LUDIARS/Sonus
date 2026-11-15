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
// (no click on DC/low frequencies). Switching the lowpass off ramps it to the
// open limit within the block; switching the highpass off keeps its cutoff and
// crossfades linearly from the filtered output to the input over the block.
// A non-finite filter state at the end of a call (NaN / Inf input) is reset.
// Processing is in place.
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
// Biquads (Robert Bristow-Johnson's cookbook). Designed in double precision.
// DesignBiquad returns both forms of the same filter:
//   - direct form (b0..a2, a0 = 1) rounded to float, the denominator nudged so
//     the float (a1, a2) are strictly inside the stability triangle;
//   - the equivalent trapezoidal state-variable form (g = tan(pi f / fs) scaled
//     for shelves, k, output mix m0..m2; A. Simper's linear trapezoidal SVF),
//     which BiquadProcess and BiquadMagnitude use. Its poles stay accurate down
//     to 1 Hz at 192 kHz (float direct-form coefficients do not: 1 + a1 + a2 ~
//     w0^2 is lost to rounding, e.g. a 10 Hz lowpass at 192 kHz gets +5 dB DC gain).
// Coefficients written by hand with only b0..a2 (g == 0) are processed in
// transposed direct form II.
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
  float b0, b1, b2, a1, a2;  // direct form, a0 normalised to 1
  float g, k, m0, m1, m2;    // SVF form; g == 0: not set (direct form is used)
};
inline constexpr BiquadCoefs kBiquadIdentity{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// freqHz is clamped to [1 Hz, 0.49 * sampleRate], q to [0.05, 100]; any
// sampleRate up to 192 kHz and beyond. gainDb (Peaking / LowShelf / HighShelf)
// is clamped to +-60 dB; shelf slope S = 1 when q <= 0.
BiquadCoefs DesignBiquad(BiquadType type, float freqHz, float q, float gainDb,
                         float sampleRate) noexcept;
// |H(e^jw)| at freqHz in double precision, from the SVF form when set (what
// BiquadProcess runs), otherwise from the direct form.
double BiquadMagnitude(const BiquadCoefs& c, double freqHz, double sampleRate) noexcept;
// Converts an octave bandwidth (Unity ParamEQ "octave range") to Q.
float OctaveBandwidthToQ(float octaves) noexcept;

struct BiquadState {
  float z1[kMaxChannels];  // SVF: ic1eq / direct form: z1
  float z2[kMaxChannels];  // SVF: ic2eq / direct form: z2
};
void BiquadReset(BiquadState& s) noexcept;
// SVF designs: g, k and m0..m2 interpolate linearly per frame from `from` to
// `to` (g, k > 0 all the way, so every intermediate filter is stable).
// Direct-form coefficients interpolate per frame (the stability triangle is
// convex). Mixing the two forms in one call uses `to` for the whole call; the
// state is not converted between forms. in and out may alias. A non-finite
// state at the end of the call (NaN / Inf input) is reset to zero; non-finite
// output samples of that call are not repaired.
void BiquadProcess(BiquadState& s, const BiquadCoefs& from, const BiquadCoefs& to,
                   const float* const* in, float* const* out, std::uint32_t channels,
                   std::uint32_t frames) noexcept;
// Flushes tiny state values to zero (no denormals without FTZ/DAZ).
void BiquadFlush(BiquadState& s, std::uint32_t channels) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_FILTER_HPP
