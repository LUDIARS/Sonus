// Pitch-preserving time stretch for voices (design.md 7 "ピッチ・速度"; CONTRACTS.md
// D-209, D-210). WSOLA (waveform-similarity overlap-add).
//
// speed = source frames advanced per output frame (1 = normal, 2 = twice as fast,
// 0.5 = half as fast), pitch unchanged. It ramps linearly inside a call like the
// resampler ratio: output frame i of n uses speed(i) = speedStart + (speedEnd -
// speedStart) * i / n. The two endpoints are clamped to [kTimeStretchMinSpeed,
// maxSpeed] BEFORE the interpolation (NaN -> 1, -Inf -> minimum, +Inf -> maxSpeed;
// D-217), so the interpolated speed is always finite. The stretcher runs at the SOURCE rate, before the resampler
// (core: source -> time stretch -> resampler, see
// spec/handoff/parallel/requests/D-10-timestretch-voice.md).
//
// Method (fixed, D-209, D-217): the output is cut into hops of H frames (H = window / 2,
// default window 30 ms). Every hop k takes a segment of 2H input frames starting
// at p_k and overlap-adds it with a sin^2 (Hann) cross-fade over H frames:
//   out[k H + j] = wf[j] x[p_{k-1} + H + j] + wr[j] x[p_k + j],  j < H,
//   wr[j] = sin^2(pi (j + 0.5) / (2 H)),  wf[j] = wr[H - 1 - j].
// The nominal position advances by speed * H per hop (32 fractional bits relative to the retained history; the
// speed of hop k is speed(i) at the output frame i where hop k starts). p_k is
// chosen in [max(0, nominal - S), nominal + S] (S = search radius, default 12 ms;
// never before the first input frame):
//   - if the natural continuation p_{k-1} + H is in that range it is taken
//     (then out[k H + j] = x[p_k + j] exactly, no cross-fade);
//   - otherwise the candidate with the largest score wins, score = C |C| / E with
//     C = sum over the channels c of sum_j x_c[p + j] x_c[p_{k-1} + H + j] (each
//     channel against its own target, j < H) and E = sum over the channels of the
//     candidate energy sum_j x_c[p + j]^2 (0 when E is 0). Channels in anti-phase
//     add instead of cancelling; a silent channel contributes nothing:
//     a coarse search every D frames on every D-th sample (D = power of two,
//     about H / 64), then every frame within +-(D - 1) of the coarse best, on
//     every sample. Ties go to the smaller position. Deterministic.
// All channels use the same p_k, so inter-channel phase is preserved exactly.
// Transients (D-217): there is no transient detection. With the fixed window and
// hop, a click can be repeated (speed < 1: at 0.5 every click comes out twice) or
// dropped / attenuated (speed > 1: at 2 about a third disappear). Intended use:
// BGM and voices stretched by about +-20..30 %.
//
// Speed 1: hop 0 starts at p_0 = 0 with p_{-1} = -H (natural), so at speed 1 from
// Init the output is the input, bit for bit, with no delay. After other speeds,
// returning to speed 1 gives the input delayed by a constant (|offset| <= S).
//
// Calling sequence per voice and block (all RT safe after Init, no allocation):
//   1. n = TimeStretchInputFrames(state, outFrames, s0, s1)      // new input frames
//   2. the source writes n frames to in[c] + TimeStretchPadFrames(state)
//   3. TimeStretchProcess(state, in, n, out, outFrames, s0, s1)   // consumes n
// in[c] must hold PadFrames + n floats; the pad is scratch the stretcher
// overwrites with its history (same convention as the resampler). The pad is a
// multiple of 16 frames. out must not overlap in.
//
// Input requirement (pure, data independent): a hop with nominal position T
// needs input up to frame floor(T) + S + 2H (exclusive), i.e. look-ahead
// S + 2H frames (TimeStretchLookaheadFrames). The input consumed by a call is
// exactly what the hops decided in it need beyond what was consumed before, so
// over a long run consumed ~= sum of speeds + look-ahead, within one INPUT
// advance (speed * H frames, at most maxSpeed * H).
// outFrames is capped at kTimeStretchMaxCallFrames per call (D-217): InputFrames
// and Process treat a larger call as that many frames; Process writes silence
// to the frames past the cap. The returned input count always fits uint32.
// TimeStretchMaxInputFrames bounds n for scratch sizing.
//
// Block splitting: with a constant speed the output is bit-identical however
// the frames are split into calls. A ramped speed split at the matching
// intermediate speeds gives the same hops up to the float rounding of speed(i).
//
// State: TimeStretchStateBytes(channels, sampleRate, maxSpeed, window, search)
// bytes, 64-byte aligned, caller owned (core: one slot per real voice, like the
// resampler pool, D-44). Init writes every byte.
#ifndef SND_DSP_TIME_STRETCH_HPP
#define SND_DSP_TIME_STRETCH_HPP

#include <cstddef>
#include <cstdint>

namespace snd::dsp {

inline constexpr float kTimeStretchMinSpeed = 0.25f;
inline constexpr float kTimeStretchMaxSpeed = 4.0f;
inline constexpr float kTimeStretchDefaultWindowMs = 30.0f;  // segment = 2 hops
inline constexpr float kTimeStretchDefaultSearchMs = 12.0f;  // +- search radius (D-217)
inline constexpr std::uint32_t kTimeStretchMaxRate = 384000;
// Largest outFrames one call handles (D-217); larger calls output silence past it.
inline constexpr std::uint32_t kTimeStretchMaxCallFrames = 1u << 20;

// Geometry derived from (sampleRate, maxSpeed, windowMs, searchMs):
//   hop H = max(16, round(rate * windowMs / 2000)), window 2H;
//   search S = round(rate * searchMs / 1000) (0 allowed; default 12 ms = 576 at 48 kHz);
//   decimation D = largest power of two <= max(1, H / 64);
//   look-ahead S + 2H; pad = round up to 16 of (2S + H + ceil(maxSpeed * H) + 2).
// windowMs is clamped to [5, 200], searchMs to [0, 50], maxSpeed to
// [1, kTimeStretchMaxSpeed] (NaN windowMs / searchMs -> the defaults, NaN maxSpeed -> 1). sampleRate must be 1..kTimeStretchMaxRate and
// channels 1..kMaxChannels (8), otherwise StateBytes is 0 and Init fails.
struct TimeStretchShape {
  std::uint32_t hopFrames;
  std::uint32_t searchFrames;
  std::uint32_t decimation;
  std::uint32_t lookaheadFrames;
  std::uint32_t padFrames;
};
bool TimeStretchGetShape(std::uint32_t sampleRate, float maxSpeed, TimeStretchShape* out,
                         float windowMs = kTimeStretchDefaultWindowMs,
                         float searchMs = kTimeStretchDefaultSearchMs) noexcept;

std::size_t TimeStretchStateBytes(std::uint32_t channels, std::uint32_t sampleRate,
                                  float maxSpeed = kTimeStretchMaxSpeed,
                                  float windowMs = kTimeStretchDefaultWindowMs,
                                  float searchMs = kTimeStretchDefaultSearchMs) noexcept;
// Not RT (computes the window table); no allocation. False on invalid
// arguments or bytes < StateBytes.
bool TimeStretchInit(void* state, std::size_t bytes, std::uint32_t channels,
                     std::uint32_t sampleRate, float maxSpeed = kTimeStretchMaxSpeed,
                     float windowMs = kTimeStretchDefaultWindowMs,
                     float searchMs = kTimeStretchDefaultSearchMs) noexcept;
// Same as a fresh Init with the same arguments. RT safe.
void TimeStretchReset(void* state) noexcept;

std::uint32_t TimeStretchChannels(const void* state) noexcept;
std::uint32_t TimeStretchPadFrames(const void* state) noexcept;
std::uint32_t TimeStretchHopFrames(const void* state) noexcept;
std::uint32_t TimeStretchLookaheadFrames(const void* state) noexcept;
// Upper bound of TimeStretchInputFrames(state, outFrames, *, *) for any state
// and speeds (scratch sizing): (ceil(outFrames / H) + 1) * (ceil(maxSpeed H) + 1)
// + look-ahead.
std::uint32_t TimeStretchMaxInputFrames(const void* state, std::uint32_t outFrames) noexcept;

// New input frames the next TimeStretchProcess with the same arguments
// consumes. Pure.
std::uint32_t TimeStretchInputFrames(const void* state, std::uint32_t outFrames, float speedStart,
                                     float speedEnd) noexcept;
// Writes outFrames frames to out[c]. Returns the frames consumed
// (== TimeStretchInputFrames). If inFrames is smaller than required, outputs
// silence, leaves the state unchanged and returns 0.
std::uint32_t TimeStretchProcess(void* state, float* const* in, std::uint32_t inFrames,
                                 float* const* out, std::uint32_t outFrames, float speedStart,
                                 float speedEnd) noexcept;

// Total input frames consumed since Init / Reset.
std::uint64_t TimeStretchConsumedFrames(const void* state) noexcept;
// Source position of the next output frame relative to the next unread input
// frame (always <= 0 after the first call; 0 after Init). Exact while inside a
// hop; at a hop boundary it is the natural continuation (the next hop may
// still jump by up to the search radius). Core's playhead in source frames is
// consumed + offset (see the request D-10).
double TimeStretchPendingOffset(const void* state) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_TIME_STRETCH_HPP
