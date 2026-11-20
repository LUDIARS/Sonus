// Pitch-preserving time stretch for voices (design.md 7 "ピッチ・速度"; CONTRACTS.md
// D-209, D-210). WSOLA (waveform-similarity overlap-add).
//
// speed = source frames advanced per output frame (1 = normal, 2 = twice as fast,
// 0.5 = half as fast), pitch unchanged. It ramps linearly inside a call like the
// resampler ratio: output frame i of n uses speed(i) = speedStart + (speedEnd -
// speedStart) * i / n. Speeds are clamped to [kTimeStretchMinSpeed, maxSpeed]
// (NaN -> 1). The stretcher runs at the SOURCE rate, before the resampler
// (core: source -> time stretch -> resampler, see
// spec/handoff/parallel/requests/D-10-timestretch-voice.md).
//
// Method (fixed, D-209): the output is cut into hops of H frames (H = window / 2,
// default window 30 ms). Every hop k takes a segment of 2H input frames starting
// at p_k and overlap-adds it with a sin^2 (Hann) cross-fade over H frames:
//   out[k H + j] = wf[j] x[p_{k-1} + H + j] + wr[j] x[p_k + j],  j < H,
//   wr[j] = sin^2(pi (j + 0.5) / (2 H)),  wf[j] = wr[H - 1 - j].
// The nominal position advances by speed * H per hop (32.32 fixed point; the
// speed of hop k is speed(i) at the output frame i where hop k starts). p_k is
// chosen in [nominal - S, nominal + S] (S = search radius, default 8 ms):
//   - if the natural continuation p_{k-1} + H is in that range it is taken
//     (then out[k H + j] = x[p_k + j] exactly, no cross-fade);
//   - otherwise the candidate with the largest normalised cross-correlation
//     between x[p + j] and x[p_{k-1} + H + j] (j < H, channels summed) wins:
//     a coarse search every D frames on every D-th sample (D = power of two,
//     about H / 64), then every frame within +-(D - 1) of the coarse best, on
//     every sample. Ties go to the smaller position. Deterministic.
// All channels use the same p_k, so inter-channel phase is preserved exactly.
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
// over a long run consumed ~= sum of speeds + look-ahead (+- one hop).
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
inline constexpr float kTimeStretchDefaultSearchMs = 8.0f;   // +- search radius
inline constexpr std::uint32_t kTimeStretchMaxRate = 384000;

// Geometry derived from (sampleRate, maxSpeed, windowMs, searchMs):
//   hop H = max(16, round(rate * windowMs / 2000)), window 2H;
//   search S = round(rate * searchMs / 1000) (0 allowed);
//   decimation D = largest power of two <= max(1, H / 64);
//   look-ahead S + 2H; pad = round up to 16 of (2S + H + ceil(maxSpeed * H) + 2).
// windowMs is clamped to [5, 200], searchMs to [0, 50], maxSpeed to
// [1, kTimeStretchMaxSpeed]. sampleRate must be 1..kTimeStretchMaxRate and
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
