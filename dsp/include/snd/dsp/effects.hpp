// Standard effects, effect chains with plugins, and DSP instance pool sizing
// (design.md 2.4, 3.3, 4.6, 7 "エフェクト", 12.3).
//
// Standard effects are not virtual: their state is plain memory from the DSP
// instance pool and they are processed per kind in batch loops
// (EffectProcessBatch). Plugins (snd_dsp_effect / snd::IDspEffect) run in the
// same chain through ProcessEffectChain.
//
// Effect type id: snd::HashId of the kind name below (bank EFFX.effectType,
// bank-format.md). The bank format defines no ids for standard effects, so
// these names are the stage-1 decision (see the dsp report / contract proposal).
//
// Parameters: EFPV values / ParamBlock entries are indexed as in the tables
// below (name, unit, range, default). Values are clamped to the range (NaN ->
// default, +-Inf -> the bound). ParamBlock start/end ramp linearly (D-26):
//   - mix values, times, cutoffs read per frame from the interpolated parameter;
//   - Distortion (drive, output, tone) and Compressor (attack, release, makeup,
//     sidechain gain) derive their gains / coefficients per frame from the
//     interpolated parameter while it ramps;
//   - Delay (dry/wet gains, damping), Limiter (release), Normalize (fade),
//     Surround (rear/centre gains, lowpass) and Reverb (all derived gains and
//     coefficients) interpolate the derived values between the block's start
//     and end values;
//   - filter coefficients (ParamEq, Lowpass, Highpass) are designed every 32
//     frames and interpolated per frame in between.
// Block splitting: with constant parameters the output is bit-identical however
// a block is split into calls. With ramping parameters, splitting a ramp at the
// matching intermediate values changes the output by less than 2e-4 (measured,
// every parameter ramping 70 % of the way to its maximum over 1024 frames split
// into 4 calls: <= 9.2e-5 for Normalize and Flanger, <= 2e-6 for the others).
// Parameters missing from a ParamBlock (index >= count, or a null block) keep
// their last value. Parameters marked [init] size buffers or set latency and
// are only read by EffectInit (from EffectDesc::initial_params).
//
// Robustness: calls with 0 channels or 0 frames do nothing. If the input or the
// output of a call contains NaN / Inf, the non-finite output samples are set to
// 0 and the state is reset at the end of the call (as EffectReset), so the next
// call is normal again. EffectInit writes every byte the instance uses (the
// pool memory may hold anything). Feedback loops are never lossless: loop gains
// are capped below 1 (Echo Decay, Delay Feedback 0.98; Chorus 0.9; Flanger 0.95).
//
//  snd.fx.reverb   (algorithmic, 8-line FDN; I3DL2 / Unity SFX Reverb set)
//    0 DryLevel         mB  -10000..0      0
//    1 Room             mB  -10000..0      -1000
//    2 RoomHF           mB  -10000..0      -100
//    3 RoomLF           mB  -10000..0      0
//    4 DecayTime        s   0.1..20        1.49
//    5 DecayHFRatio     -   0.1..2         0.83   (values > 1 behave as 1)
//    6 Reflections      mB  -10000..1000   -2602
//    7 ReflectionsDelay s   0..0.3         0.007
//    8 Reverb           mB  -10000..2000   200
//    9 ReverbDelay      s   0..0.1         0.011
//   10 Diffusion        %   0..100         100
//   11 Density          %   0..100         100
//   12 HFReference      Hz  20..20000      5000
//   13 LFReference      Hz  20..1000       250
//    (I3DL2 RoomRolloffFactor is distance attenuation and belongs to core's 3D.)
//  snd.fx.echo     (Unity Echo)
//    0 Delay ms 1..5000 500 | 1 Decay 0..0.98 0.5 | 2 DryMix 0..1 1 | 3 WetMix 0..1 1
//    4 MaxDelay ms 10..5000 1000 [init]
//    (Delay is clamped to MaxDelay. Tail length after the input stops: about
//     Delay * ln(peak / 1e-5) / -ln(Decay), e.g. 500 ms, Decay 0.98, 0 dBFS: ~5 min.)
//  snd.fx.delay    (feedback delay with damping and ping-pong)
//    0 Time ms 0..4000 250 | 1 Feedback 0..0.98 0.3 | 2 DryDb -96..0 0 | 3 WetDb -96..0 -6
//    4 DampingHz 500..20000 20000 | 5 PingPong 0..1 0 | 6 MaxDelay ms 10..4000 1000 [init]
//    (Time below one frame, including 0 ms, acts as 1 frame; Time is clamped to
//     MaxDelay. DryDb / WetDb -96 is -96 dB, not off. Tail length after the input
//     stops: about Time * ln(peak / 1e-5) / -ln(Feedback): 250 ms at 0.3: 2.6 s,
//     at 0.9: 27 s, at 0.98: ~2.5 min from 0 dBFS.)
//  snd.fx.chorus   (3 modulated taps, 120 deg apart; Unity Chorus)
//    0 DryMix 0..1 0.5 | 1 WetMix 0..1 0.5 | 2 Delay ms 1..50 20 | 3 Rate Hz 0..10 0.8
//    4 Depth 0..1 0.3 | 5 Feedback -0.9..0.9 0
//  snd.fx.flanger  (Unity Flange)
//    0 DryMix 0..1 0.45 | 1 WetMix 0..1 0.55 | 2 Delay ms 0.1..10 1 | 3 Rate Hz 0..20 0.1
//    4 Depth 0..1 1 | 5 Feedback -0.95..0.95 0.5
//  snd.fx.distortion (soft clipper; Unity Distortion "Level" = Drive)
//    0 Drive 0..1 0.5 | 1 Mix 0..1 1 | 2 OutputDb -48..12 0 | 3 ToneHz 500..20000 20000
//  snd.fx.compressor (stereo-linked, soft knee; sidechain input for ducking 4.6)
//    0 ThresholdDb -60..0 -20 | 1 Ratio 1..50 4 | 2 AttackMs 0.05..500 10
//    3 ReleaseMs 1..5000 100 | 4 KneeDb 0..24 6 | 5 MakeupDb 0..30 0
//    6 SidechainDb -24..24 0 | 7 UseSidechain 0..1 1
//    (Attack / release are one-pole time constants on the gain change in dB: the
//     remaining change falls to 1/e (-63 %) after AttackMs / ReleaseMs; peak
//     detector, max |x| over the channels or the sidechain.)
//  snd.fx.limiter  (look-ahead brickwall)
//    0 CeilingDb -30..0 -0.3 | 1 ReleaseMs 1..1000 50 | 2 LookaheadMs 0..10 2 [init]
//    3 InputGainDb 0..24 0
//  snd.fx.parameq  (4 bands; band b uses params 4b..4b+3)
//    4b+0 Type 0..8 (0 off, 1 peaking, 2 low shelf, 3 high shelf, 4 LP, 5 HP, 6 BP, 7 notch,
//                    8 allpass: stage 3 D-135, appended)
//    ("Various filters" of design 7 = the Lowpass / Highpass kinds and these band types, D-135.)
//    4b+1 FreqHz 20..20000 (100, 500, 2000, 8000) | 4b+2 Q 0.1..20 0.707 | 4b+3 GainDb -24..24 0
//    defaults: all bands peaking 0 dB (transparent)
//  snd.fx.lowpass  / snd.fx.highpass (12 dB/oct; Unity Lowpass/Highpass Simple+Resonance)
//    0 CutoffHz 10..22000 5000 | 1 Resonance (Q) 0.5..10 1
//  snd.fx.pitchshift (two-tap delay-line pitch shifter)
//    0 Pitch 0.5..2 1 | 1 WindowMs 10..200 50 | 2 Mix 0..1 1
//    (Latency WindowMs / 2, reported by EffectLatencyFrames; the dry part of Mix
//     is delayed by the same amount, so Pitch 1 = the input delayed, any Mix.)
//  snd.fx.normalize (Unity Normalize)
//    0 FadeInMs 0..20000 5000 | 1 LowestVolume 0..1 0.1 | 2 MaximumAmp 1..100000 20
//  snd.fx.surround (pseudo surround / stereo widener; CRI "サラウンダー" equivalent)
//    0 Width 0..2 1 | 1 RearDb -60..0 -6 | 2 RearDelayMs 0..50 15 | 3 RearLowpassHz 500..20000 7000
//    4 CenterDb -60..0 -60  (rears/centre are fed for 6 / 8 channel layouts, pan.hpp order)
//
// Tail (2.4): EffectIsTailDone() is true once the effect outputs only silence
// for silent input (internal signal below -100 dBFS for the longest internal
// delay). Core stops feeding a bus, processes until the tail is done, then
// bypasses. Feedback buffers are cleared at that point, so no denormals build
// up even without FTZ/DAZ.
#ifndef SND_DSP_EFFECTS_HPP
#define SND_DSP_EFFECTS_HPP

#include <cstddef>
#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/types.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/id.hpp"

namespace snd::dsp {

enum class EffectKind : std::uint32_t {
  Reverb = 0,
  Echo = 1,
  Delay = 2,
  Chorus = 3,
  Flanger = 4,
  Distortion = 5,
  Compressor = 6,
  Limiter = 7,
  ParamEq = 8,
  Lowpass = 9,
  Highpass = 10,
  PitchShifter = 11,
  Normalize = 12,
  Surround = 13,
  Count = 14,
};

inline constexpr Id kEffectReverb = HashId("snd.fx.reverb");
inline constexpr Id kEffectEcho = HashId("snd.fx.echo");
inline constexpr Id kEffectDelay = HashId("snd.fx.delay");
inline constexpr Id kEffectChorus = HashId("snd.fx.chorus");
inline constexpr Id kEffectFlanger = HashId("snd.fx.flanger");
inline constexpr Id kEffectDistortion = HashId("snd.fx.distortion");
inline constexpr Id kEffectCompressor = HashId("snd.fx.compressor");
inline constexpr Id kEffectLimiter = HashId("snd.fx.limiter");
inline constexpr Id kEffectParamEq = HashId("snd.fx.parameq");
inline constexpr Id kEffectLowpass = HashId("snd.fx.lowpass");
inline constexpr Id kEffectHighpass = HashId("snd.fx.highpass");
inline constexpr Id kEffectPitchShifter = HashId("snd.fx.pitchshift");
inline constexpr Id kEffectNormalize = HashId("snd.fx.normalize");
inline constexpr Id kEffectSurround = HashId("snd.fx.surround");

inline constexpr std::uint32_t kMaxEffectParams = 16;

enum EffectParamFlags : std::uint32_t {
  kParamInitOnly = 1u << 0,  // read by EffectInit only
  kParamInteger = 1u << 1,   // enumerated value (rounded)
};

struct EffectParamInfo {
  const char* name;
  const char* unit;  // "dB", "mB", "ms", "s", "Hz", "%", "" ...
  float minValue;
  float maxValue;
  float defaultValue;
  std::uint32_t flags;  // EffectParamFlags
};

enum EffectInfoFlags : std::uint32_t {
  kEffectHasSidechain = 1u << 0,  // reads EffectSidechain when given
  kEffectHasLatency = 1u << 1,    // delays the dry signal (limiter look-ahead)
};

struct EffectInfo {
  Id typeId;
  EffectKind kind;
  const char* name;  // HashId(name) == typeId
  std::uint32_t paramCount;
  const EffectParamInfo* params;
  std::uint32_t flags;  // EffectInfoFlags
};

const EffectInfo& GetEffectInfo(EffectKind kind) noexcept;
// nullptr for unknown ids (plugins).
const EffectInfo* FindEffect(Id typeId) noexcept;

// Sidechain input (compressor ducking). Channels are mixed by peak (max |x|).
struct EffectSidechain {
  const float* const* channels;
  std::uint32_t channelCount;
};

// --- Single instance (state = DSP instance pool memory, 64-byte aligned) ---
// Bytes for the instance (multiple of 64), 0 if the type is not a standard
// effect or desc is invalid (channels 0 or > kMaxChannels, max_frames 0).
// Rate-dependent buffers are sized for `sampleRate` (the bank's target rate).
// Contract 1.1 (D-48): the rate argument is kept as the rate; desc.sample_rate
// must be 0 (unset) or equal to it, otherwise these functions fail (0 / false).
std::uint32_t EffectStateBytes(const EffectDesc& desc, std::uint32_t sampleRate) noexcept;
// Normal worker (bank load). `bytes` = memory available (>= EffectStateBytes).
// If sampleRate is higher than the one used for sizing, delay capacities are
// clamped to what fits. Returns false if the type is unknown or memory too small.
bool EffectInit(void* state, std::size_t bytes, const EffectDesc& desc,
                std::uint32_t sampleRate) noexcept;
// RT. in/out may alias. channels <= desc.channels; frames may exceed
// desc.max_frames (processed in sub-blocks). params may be null (hold values).
void EffectProcess(void* state, const float* const* in, float* const* out, std::uint32_t channels,
                   std::uint32_t frames, const ParamBlock* params,
                   const EffectSidechain* sidechain = nullptr) noexcept;
bool EffectIsTailDone(const void* state) noexcept;
// Same state as a fresh EffectInit with the current parameter values (history,
// envelopes, LFO phases and latency); the tail is done afterwards.
void EffectReset(void* state) noexcept;
EffectKind EffectStateKind(const void* state) noexcept;
// Current (last end) parameter value, for tools and tests.
float EffectParamValue(const void* state, std::uint32_t index) noexcept;
// Latency in frames: limiter look-ahead - 1, pitch shifter WindowMs / 2, 0 for the others.
std::uint32_t EffectLatencyFrames(const void* state) noexcept;

// --- Batch: one call per kind, no per-instance dispatch (12.3) ---
struct EffectBatchItem {
  void* state;
  const float* const* in;
  float* const* out;
  std::uint32_t channels;
  std::uint32_t frames;
  const ParamBlock* params;            // may be null
  const EffectSidechain* sidechain;    // may be null
};
// Every item must hold a state of `kind`.
void EffectProcessBatch(EffectKind kind, const EffectBatchItem* items, std::uint32_t count) noexcept;

// --- Chains (bus effect ranges, voice effects): standard and plugin effects ---
struct EffectSlot {
  Id typeId;                         // standard or plugin type
  const snd_dsp_effect* plugin;      // non-null for plugins (Config.plugins)
  void* state;                       // instance memory
  const ParamBlock* params;          // may be null
  const EffectSidechain* sidechain;  // standard effects only
  std::uint32_t bypass;              // 1 = skip (2.4 LOD bypass after the tail)
  std::uint32_t reserved;
};
// Processes the slots in order, in place on buffers[0..channels).
void ProcessEffectChain(const EffectSlot* slots, std::uint32_t count, float* const* buffers,
                        std::uint32_t channels, std::uint32_t frames) noexcept;
bool EffectSlotIsTailDone(const EffectSlot& slot) noexcept;

// Plugin helpers (normal worker): size and init through the plugin table.
// PluginStateBytes passes `desc` as is (set desc.sample_rate first, D-48);
// EffectInstanceBytes / ComputeDspPoolNeed fill it from their rate argument.
std::uint32_t PluginStateBytes(const snd_dsp_effect& plugin, const EffectDesc& desc) noexcept;

// --- DSP instance pool sizing (3.3; buildtool -> DSPN, bank load) ---
// Bytes of one instance, rounded up to 64 (EFFX.stateBytes). A plugin in
// `plugins` (may be null) with the type id wins over a standard effect of the
// same id (D-31: registration overrides built-ins, as for codecs); otherwise
// standard effects use EffectStateBytes. 0 if the type is unknown.
std::uint32_t EffectInstanceBytes(const EffectDesc& desc, std::uint32_t sampleRate,
                                  const snd_dsp_effect* plugins, std::uint32_t pluginCount) noexcept;
// Sum over descs (each rounded to 64) = bank_format::DspPoolNeed. `unknown`
// (optional) receives the number of descs whose type could not be sized.
bank_format::DspPoolNeed ComputeDspPoolNeed(const EffectDesc* descs, std::uint32_t count,
                                            std::uint32_t sampleRate,
                                            const snd_dsp_effect* plugins,
                                            std::uint32_t pluginCount,
                                            std::uint32_t* unknown = nullptr) noexcept;

// Fills `values` (paramCount entries) with the defaults of `kind`.
void EffectDefaultParams(EffectKind kind, float* values) noexcept;

}  // namespace snd::dsp

#endif  // SND_DSP_EFFECTS_HPP
