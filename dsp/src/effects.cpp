// Standard effect registry, parameter tables, dispatch, chains and pool sizing.
#include "snd/dsp/effects.hpp"

#include <cmath>
#include <cstring>

#include "fx_common.hpp"

namespace snd::dsp {

namespace {

using fx::FxConfig;
using fx::FxHeader;
using fx::FxIo;
using fx::FxParams;

constexpr std::uint32_t kInit = kParamInitOnly;
constexpr std::uint32_t kInt = kParamInteger;

// ---------------------------------------------------------------------------
// Parameter tables (documented in effects.hpp)
// ---------------------------------------------------------------------------
constexpr EffectParamInfo kReverbParams[] = {
    {"DryLevel", "mB", -10000.0f, 0.0f, 0.0f, 0},
    {"Room", "mB", -10000.0f, 0.0f, -1000.0f, 0},
    {"RoomHF", "mB", -10000.0f, 0.0f, -100.0f, 0},
    {"RoomLF", "mB", -10000.0f, 0.0f, 0.0f, 0},
    {"DecayTime", "s", 0.1f, 20.0f, 1.49f, 0},
    {"DecayHFRatio", "", 0.1f, 2.0f, 0.83f, 0},
    {"Reflections", "mB", -10000.0f, 1000.0f, -2602.0f, 0},
    {"ReflectionsDelay", "s", 0.0f, 0.3f, 0.007f, 0},
    {"Reverb", "mB", -10000.0f, 2000.0f, 200.0f, 0},
    {"ReverbDelay", "s", 0.0f, 0.1f, 0.011f, 0},
    {"Diffusion", "%", 0.0f, 100.0f, 100.0f, 0},
    {"Density", "%", 0.0f, 100.0f, 100.0f, 0},
    {"HFReference", "Hz", 20.0f, 20000.0f, 5000.0f, 0},
    {"LFReference", "Hz", 20.0f, 1000.0f, 250.0f, 0},
};
constexpr EffectParamInfo kEchoParams[] = {
    {"Delay", "ms", 1.0f, 5000.0f, 500.0f, 0},
    {"Decay", "", 0.0f, 0.98f, 0.5f, 0},  // capped: no lossless loop (decision a)
    {"DryMix", "", 0.0f, 1.0f, 1.0f, 0},
    {"WetMix", "", 0.0f, 1.0f, 1.0f, 0},
    {"MaxDelay", "ms", 10.0f, 5000.0f, 1000.0f, kInit},
};
constexpr EffectParamInfo kDelayParams[] = {
    {"Time", "ms", 0.0f, 4000.0f, 250.0f, 0},
    {"Feedback", "", 0.0f, 0.98f, 0.3f, 0},
    {"DryDb", "dB", -96.0f, 0.0f, 0.0f, 0},
    {"WetDb", "dB", -96.0f, 0.0f, -6.0f, 0},
    {"DampingHz", "Hz", 500.0f, 20000.0f, 20000.0f, 0},
    {"PingPong", "", 0.0f, 1.0f, 0.0f, 0},
    {"MaxDelay", "ms", 10.0f, 4000.0f, 1000.0f, kInit},
};
constexpr EffectParamInfo kChorusParams[] = {
    {"DryMix", "", 0.0f, 1.0f, 0.5f, 0},
    {"WetMix", "", 0.0f, 1.0f, 0.5f, 0},
    {"Delay", "ms", 1.0f, 50.0f, 20.0f, 0},
    {"Rate", "Hz", 0.0f, 10.0f, 0.8f, 0},
    {"Depth", "", 0.0f, 1.0f, 0.3f, 0},
    {"Feedback", "", -0.9f, 0.9f, 0.0f, 0},
};
constexpr EffectParamInfo kFlangerParams[] = {
    {"DryMix", "", 0.0f, 1.0f, 0.45f, 0},
    {"WetMix", "", 0.0f, 1.0f, 0.55f, 0},
    {"Delay", "ms", 0.1f, 10.0f, 1.0f, 0},
    {"Rate", "Hz", 0.0f, 20.0f, 0.1f, 0},
    {"Depth", "", 0.0f, 1.0f, 1.0f, 0},
    {"Feedback", "", -0.95f, 0.95f, 0.5f, 0},
};
constexpr EffectParamInfo kDistortionParams[] = {
    {"Drive", "", 0.0f, 1.0f, 0.5f, 0},
    {"Mix", "", 0.0f, 1.0f, 1.0f, 0},
    {"OutputDb", "dB", -48.0f, 12.0f, 0.0f, 0},
    {"ToneHz", "Hz", 500.0f, 20000.0f, 20000.0f, 0},
};
constexpr EffectParamInfo kCompressorParams[] = {
    {"ThresholdDb", "dB", -60.0f, 0.0f, -20.0f, 0},
    {"Ratio", "", 1.0f, 50.0f, 4.0f, 0},
    {"AttackMs", "ms", 0.05f, 500.0f, 10.0f, 0},
    {"ReleaseMs", "ms", 1.0f, 5000.0f, 100.0f, 0},
    {"KneeDb", "dB", 0.0f, 24.0f, 6.0f, 0},
    {"MakeupDb", "dB", 0.0f, 30.0f, 0.0f, 0},
    {"SidechainDb", "dB", -24.0f, 24.0f, 0.0f, 0},
    {"UseSidechain", "", 0.0f, 1.0f, 1.0f, kInt},
};
constexpr EffectParamInfo kLimiterParams[] = {
    {"CeilingDb", "dB", -30.0f, 0.0f, -0.3f, 0},
    {"ReleaseMs", "ms", 1.0f, 1000.0f, 50.0f, 0},
    {"LookaheadMs", "ms", 0.0f, 10.0f, 2.0f, kInit},
    {"InputGainDb", "dB", 0.0f, 24.0f, 0.0f, 0},
};
constexpr EffectParamInfo kParamEqParams[] = {
    {"Type1", "", 0.0f, 7.0f, 1.0f, kInt},   {"Freq1", "Hz", 20.0f, 20000.0f, 100.0f, 0},
    {"Q1", "", 0.1f, 20.0f, 0.707f, 0},      {"Gain1", "dB", -24.0f, 24.0f, 0.0f, 0},
    {"Type2", "", 0.0f, 7.0f, 1.0f, kInt},   {"Freq2", "Hz", 20.0f, 20000.0f, 500.0f, 0},
    {"Q2", "", 0.1f, 20.0f, 0.707f, 0},      {"Gain2", "dB", -24.0f, 24.0f, 0.0f, 0},
    {"Type3", "", 0.0f, 7.0f, 1.0f, kInt},   {"Freq3", "Hz", 20.0f, 20000.0f, 2000.0f, 0},
    {"Q3", "", 0.1f, 20.0f, 0.707f, 0},      {"Gain3", "dB", -24.0f, 24.0f, 0.0f, 0},
    {"Type4", "", 0.0f, 7.0f, 1.0f, kInt},   {"Freq4", "Hz", 20.0f, 20000.0f, 8000.0f, 0},
    {"Q4", "", 0.1f, 20.0f, 0.707f, 0},      {"Gain4", "dB", -24.0f, 24.0f, 0.0f, 0},
};
constexpr EffectParamInfo kLowpassParams[] = {
    {"CutoffHz", "Hz", 10.0f, 22000.0f, 5000.0f, 0},
    {"Resonance", "", 0.5f, 10.0f, 1.0f, 0},
};
constexpr EffectParamInfo kHighpassParams[] = {
    {"CutoffHz", "Hz", 10.0f, 22000.0f, 5000.0f, 0},
    {"Resonance", "", 0.5f, 10.0f, 1.0f, 0},
};
constexpr EffectParamInfo kPitchParams[] = {
    {"Pitch", "", 0.5f, 2.0f, 1.0f, 0},
    {"WindowMs", "ms", 10.0f, 200.0f, 50.0f, 0},
    {"Mix", "", 0.0f, 1.0f, 1.0f, 0},
};
constexpr EffectParamInfo kNormalizeParams[] = {
    {"FadeInMs", "ms", 0.0f, 20000.0f, 5000.0f, 0},
    {"LowestVolume", "", 0.0f, 1.0f, 0.1f, 0},
    {"MaximumAmp", "", 1.0f, 100000.0f, 20.0f, 0},
};
constexpr EffectParamInfo kSurroundParams[] = {
    {"Width", "", 0.0f, 2.0f, 1.0f, 0},
    {"RearDb", "dB", -60.0f, 0.0f, -6.0f, 0},
    {"RearDelayMs", "ms", 0.0f, 50.0f, 15.0f, 0},
    {"RearLowpassHz", "Hz", 500.0f, 20000.0f, 7000.0f, 0},
    {"CenterDb", "dB", -60.0f, 0.0f, -60.0f, 0},
};
constexpr EffectParamInfo kConvolutionParams[] = {  // convolution.hpp
    {"IrSlot", "", 0.0f, 1024.0f, 0.0f, kInt},
    {"DryMix", "", 0.0f, 1.0f, 0.0f, 0},
    {"WetMix", "", 0.0f, 1.0f, 1.0f, 0},
    {"WetGainDb", "dB", -48.0f, 24.0f, 0.0f, 0},
    {"PartitionFrames", "", 0.0f, 4096.0f, 0.0f, kInit | kInt},
    {"ZeroLatency", "", 0.0f, 1.0f, 1.0f, kInit | kInt},
    {"MaxIrMs", "ms", 10.0f, 10000.0f, 2000.0f, kInit},
};

#define SND_FX_INFO(Kind, Id, Name, Params, Flags) \
  EffectInfo { Id, EffectKind::Kind, Name, sizeof(Params) / sizeof(Params[0]), Params, Flags }

constexpr EffectInfo kInfos[] = {
    SND_FX_INFO(Reverb, kEffectReverb, "snd.fx.reverb", kReverbParams, 0),
    SND_FX_INFO(Echo, kEffectEcho, "snd.fx.echo", kEchoParams, 0),
    SND_FX_INFO(Delay, kEffectDelay, "snd.fx.delay", kDelayParams, 0),
    SND_FX_INFO(Chorus, kEffectChorus, "snd.fx.chorus", kChorusParams, 0),
    SND_FX_INFO(Flanger, kEffectFlanger, "snd.fx.flanger", kFlangerParams, 0),
    SND_FX_INFO(Distortion, kEffectDistortion, "snd.fx.distortion", kDistortionParams, 0),
    SND_FX_INFO(Compressor, kEffectCompressor, "snd.fx.compressor", kCompressorParams,
                kEffectHasSidechain),
    SND_FX_INFO(Limiter, kEffectLimiter, "snd.fx.limiter", kLimiterParams, kEffectHasLatency),
    SND_FX_INFO(ParamEq, kEffectParamEq, "snd.fx.parameq", kParamEqParams, 0),
    SND_FX_INFO(Lowpass, kEffectLowpass, "snd.fx.lowpass", kLowpassParams, 0),
    SND_FX_INFO(Highpass, kEffectHighpass, "snd.fx.highpass", kHighpassParams, 0),
    SND_FX_INFO(PitchShifter, kEffectPitchShifter, "snd.fx.pitchshift", kPitchParams, 0),
    SND_FX_INFO(Normalize, kEffectNormalize, "snd.fx.normalize", kNormalizeParams, 0),
    SND_FX_INFO(Surround, kEffectSurround, "snd.fx.surround", kSurroundParams, 0),
    SND_FX_INFO(Convolution, kEffectConvolution, "snd.fx.convolution", kConvolutionParams,
                kEffectHasLatency),
};
#undef SND_FX_INFO
static_assert(sizeof(kInfos) / sizeof(kInfos[0]) == static_cast<std::size_t>(EffectKind::Count));

// Ids must be distinct and match the names (checked once at compile time).
constexpr bool IdsConsistent() {
  for (std::size_t i = 0; i < sizeof(kInfos) / sizeof(kInfos[0]); ++i) {
    if (static_cast<std::size_t>(kInfos[i].kind) != i) return false;
    if (kInfos[i].paramCount > kMaxEffectParams) return false;
    std::size_t len = 0;
    while (kInfos[i].name[len] != '\0') ++len;
    if (HashId(std::string_view(kInfos[i].name, len)) != kInfos[i].typeId) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (kInfos[j].typeId == kInfos[i].typeId) return false;
  }
  return true;
}
static_assert(IdsConsistent(), "standard effect ids");

// Per-kind non-RT entry points.
struct KindOps {
  std::size_t (*layout)(void*, const FxConfig&) noexcept;
  void (*init)(void*, const FxConfig&) noexcept;
  void (*reset)(void*) noexcept;
};
#define SND_FX_OPS(Name) KindOps{&fx::Name##Layout, &fx::Name##Init, &fx::Name##Reset}
constexpr KindOps kOps[] = {
    SND_FX_OPS(Reverb),     SND_FX_OPS(Echo),       SND_FX_OPS(Delay),
    SND_FX_OPS(Chorus),     SND_FX_OPS(Flanger),    SND_FX_OPS(Distortion),
    SND_FX_OPS(Compressor), SND_FX_OPS(Limiter),    SND_FX_OPS(ParamEq),
    SND_FX_OPS(Lowpass),    SND_FX_OPS(Highpass),   SND_FX_OPS(PitchShifter),
    SND_FX_OPS(Normalize),  SND_FX_OPS(Surround),
    SND_FX_OPS(Convolution),
};
#undef SND_FX_OPS

float ClampParam(const EffectParamInfo& p, float v) noexcept {
  if (!(v == v)) return p.defaultValue;  // NaN
  if (v < p.minValue) v = p.minValue;
  if (v > p.maxValue) v = p.maxValue;
  if (p.flags & kParamInteger) v = std::nearbyint(v);
  return v;
}

bool MakeConfig(const EffectInfo& info, const EffectDesc& desc, std::uint32_t sizingRate,
                std::uint32_t sampleRate, FxConfig* cfg) noexcept {
  if (desc.channels == 0 || desc.channels > kMaxChannels || desc.max_frames == 0) return false;
  if (sizingRate == 0 || sampleRate == 0) return false;
  // D-48: desc.sample_rate is unset (0) or the same rate as the argument.
  if (desc.sample_rate != 0u && desc.sample_rate != sampleRate) return false;
  cfg->channels = desc.channels;
  cfg->sizingRate = sizingRate;
  cfg->sampleRate = sampleRate;
  cfg->maxFrames = desc.max_frames;
  for (std::uint32_t i = 0; i < kMaxEffectParams; ++i) cfg->init[i] = 0.0f;
  for (std::uint32_t i = 0; i < info.paramCount; ++i) {
    float v = info.params[i].defaultValue;
    if (desc.initial_params != nullptr && i < desc.param_count) v = desc.initial_params[i];
    cfg->init[i] = ClampParam(info.params[i], v);
  }
  return true;
}

FxHeader* Header(void* s) noexcept { return static_cast<FxHeader*>(s); }
const FxHeader* Header(const void* s) noexcept { return static_cast<const FxHeader*>(s); }

void LoadParams(FxHeader& h, const EffectInfo& info, const ParamBlock* pb, FxParams* p) noexcept {
  for (std::uint32_t i = 0; i < info.paramCount; ++i) {
    const EffectParamInfo& pi = info.params[i];
    if (pb != nullptr && i < pb->count && pb->start != nullptr && pb->end != nullptr &&
        !(pi.flags & kParamInitOnly)) {
      p->s[i] = ClampParam(pi, pb->start[i]);
      p->e[i] = ClampParam(pi, pb->end[i]);
    } else {
      p->s[i] = p->e[i] = h.cur[i];
    }
    h.cur[i] = p->e[i];
  }
}

void ProcessKind(EffectKind kind, void* state, const FxIo& io, const FxParams& p) noexcept {
  switch (kind) {
    case EffectKind::Reverb: fx::ReverbProcess(state, io, p); break;
    case EffectKind::Echo: fx::EchoProcess(state, io, p); break;
    case EffectKind::Delay: fx::DelayProcess(state, io, p); break;
    case EffectKind::Chorus: fx::ChorusProcess(state, io, p); break;
    case EffectKind::Flanger: fx::FlangerProcess(state, io, p); break;
    case EffectKind::Distortion: fx::DistortionProcess(state, io, p); break;
    case EffectKind::Compressor: fx::CompressorProcess(state, io, p); break;
    case EffectKind::Limiter: fx::LimiterProcess(state, io, p); break;
    case EffectKind::ParamEq: fx::ParamEqProcess(state, io, p); break;
    case EffectKind::Lowpass: fx::LowpassProcess(state, io, p); break;
    case EffectKind::Highpass: fx::HighpassProcess(state, io, p); break;
    case EffectKind::PitchShifter: fx::PitchShifterProcess(state, io, p); break;
    case EffectKind::Normalize: fx::NormalizeProcess(state, io, p); break;
    case EffectKind::Surround: fx::SurroundProcess(state, io, p); break;
    case EffectKind::Convolution: fx::ConvolutionProcess(state, io, p); break;
    case EffectKind::Count: break;
  }
}

// Processes one instance, splitting into sub-blocks of maxFrames if needed.
void ReinitKeepingParams(void* state) noexcept;

inline void ProcessCore(EffectKind kind, const EffectInfo& info, void* state,
                        const float* const* in, float* const* out, std::uint32_t channels,
                        std::uint32_t frames, const ParamBlock* pb,
                        const EffectSidechain* sc) noexcept {
  FxHeader& h = *Header(state);
  FxParams p;
  LoadParams(h, info, pb, &p);
  if (frames <= h.maxFrames) {
    ProcessKind(kind, state, FxIo{in, out, channels, frames, sc}, p);
    return;
  }
  // Long call: sub-blocks with parameters interpolated at the boundaries.
  const float* inp[kMaxChannels];
  float* outp[kMaxChannels];
  const float* scp[kMaxChannels];
  EffectSidechain sub{scp, sc != nullptr ? sc->channelCount : 0u};
  for (std::uint32_t done = 0; done < frames;) {
    const std::uint32_t n = frames - done < h.maxFrames ? frames - done : h.maxFrames;
    const float t0 = static_cast<float>(done) / static_cast<float>(frames);
    const float t1 = static_cast<float>(done + n) / static_cast<float>(frames);
    FxParams q;
    for (std::uint32_t i = 0; i < info.paramCount; ++i) {
      q.s[i] = p.At(i, t0);
      q.e[i] = p.At(i, t1);
    }
    for (std::uint32_t c = 0; c < channels; ++c) {
      inp[c] = in[c] + done;
      outp[c] = out[c] + done;
    }
    for (std::uint32_t c = 0; c < sub.channelCount && c < kMaxChannels; ++c) scp[c] = sc->channels[c] + done;
    ProcessKind(kind, state, FxIo{inp, outp, channels, n, sc != nullptr ? &sub : nullptr}, q);
    done += n;
  }
}

// Every entry point goes through here: empty calls do nothing; non-finite
// input or output resets the state (as a fresh EffectInit with the current
// parameters) at the end of the call and non-finite output samples become 0.
inline void ProcessOne(EffectKind kind, const EffectInfo& info, void* state,
                       const float* const* in, float* const* out, std::uint32_t channels,
                       std::uint32_t frames, const ParamBlock* pb,
                       const EffectSidechain* sc) noexcept {
  FxHeader& h = *Header(state);
  if (frames == 0 || channels == 0) return;
  if (channels > h.channels) channels = h.channels;
  bool bad = false;
  for (std::uint32_t c = 0; c < channels && !bad; ++c) bad = HasNonFinite(in[c], frames);
  ProcessCore(kind, info, state, in, out, channels, frames, pb, sc);
  for (std::uint32_t c = 0; c < channels; ++c) {
    float* y = out[c];
    for (std::uint32_t i = 0; i < frames; ++i) {
      if (!std::isfinite(y[i])) {
        y[i] = 0.0f;
        bad = true;
      }
    }
  }
  if (bad) ReinitKeepingParams(state);
}

std::uint32_t RoundUp64(std::size_t n) noexcept {
  return static_cast<std::uint32_t>(detail::AlignUp(n, 64));
}

}  // namespace

const EffectInfo& GetEffectInfo(EffectKind kind) noexcept {
  const std::uint32_t k = static_cast<std::uint32_t>(kind);
  return kInfos[k < static_cast<std::uint32_t>(EffectKind::Count) ? k : 0];
}

const EffectInfo* FindEffect(Id typeId) noexcept {
  for (const EffectInfo& i : kInfos) {
    if (i.typeId == typeId) return &i;
  }
  return nullptr;
}

void EffectDefaultParams(EffectKind kind, float* values) noexcept {
  const EffectInfo& info = GetEffectInfo(kind);
  for (std::uint32_t i = 0; i < info.paramCount; ++i) values[i] = info.params[i].defaultValue;
}

std::uint32_t EffectStateBytes(const EffectDesc& desc, std::uint32_t sampleRate) noexcept {
  const EffectInfo* info = FindEffect(desc.effect_type);
  if (info == nullptr) return 0;
  FxConfig cfg;
  if (!MakeConfig(*info, desc, sampleRate, sampleRate, &cfg)) return 0;
  return RoundUp64(kOps[static_cast<std::uint32_t>(info->kind)].layout(nullptr, cfg));
}

namespace {
// Writes every byte the instance uses (the pool memory may hold anything).
void InitWith(void* state, std::size_t bytes, std::size_t need, const EffectInfo& info,
              const FxConfig& cfg) noexcept {
  const KindOps& ops = kOps[static_cast<std::uint32_t>(info.kind)];
  std::memset(state, 0, need);
  ops.layout(state, cfg);
  FxHeader& h = *Header(state);
  h.magic = fx::kFxMagic;
  h.kind = static_cast<std::uint32_t>(info.kind);
  h.channels = cfg.channels;
  h.sampleRate = cfg.sampleRate;
  h.sizingRate = cfg.sizingRate;
  h.maxFrames = cfg.maxFrames;
  h.bytes = static_cast<std::uint32_t>(bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : bytes);
  h.paramCount = info.paramCount;
  for (std::uint32_t i = 0; i < kMaxEffectParams; ++i) h.cur[i] = cfg.init[i];
  ops.init(state, cfg);
  h.silentFrames = h.tailFrames;  // nothing sounding yet
  h.cleared = 1;
}

void ReinitKeepingParams(void* state) noexcept {
  const FxHeader h = *Header(state);
  const std::uint32_t k = h.kind < static_cast<std::uint32_t>(EffectKind::Count) ? h.kind : 0u;
  const EffectInfo& info = kInfos[k];
  FxConfig cfg;
  cfg.channels = h.channels;
  cfg.sizingRate = h.sizingRate;
  cfg.sampleRate = h.sampleRate;
  cfg.maxFrames = h.maxFrames;
  for (std::uint32_t i = 0; i < kMaxEffectParams; ++i) cfg.init[i] = h.cur[i];
  const std::size_t need = kOps[k].layout(nullptr, cfg);
  InitWith(state, h.bytes, need, info, cfg);
}
}  // namespace

bool EffectInit(void* state, std::size_t bytes, const EffectDesc& desc,
                std::uint32_t sampleRate) noexcept {
  const EffectInfo* info = FindEffect(desc.effect_type);
  if (info == nullptr || state == nullptr) return false;
  const KindOps& ops = kOps[static_cast<std::uint32_t>(info->kind)];
  FxConfig cfg;
  if (!MakeConfig(*info, desc, sampleRate, sampleRate, &cfg)) return false;
  // Find the largest sizing rate that fits (higher runtime rate than the bank
  // was sized for: capacities shrink in proportion, delays are clamped).
  std::size_t need = ops.layout(nullptr, cfg);
  if (need > bytes) {
    std::uint32_t lo = 1000, hi = sampleRate;
    while (lo < hi) {
      const std::uint32_t mid = lo + (hi - lo + 1) / 2;
      cfg.sizingRate = mid;
      if (ops.layout(nullptr, cfg) <= bytes) lo = mid; else hi = mid - 1;
    }
    cfg.sizingRate = lo;
    need = ops.layout(nullptr, cfg);
    if (need > bytes) return false;
  }
  InitWith(state, bytes, need, *info, cfg);
  return true;
}

void EffectProcess(void* state, const float* const* in, float* const* out, std::uint32_t channels,
                   std::uint32_t frames, const ParamBlock* params,
                   const EffectSidechain* sidechain) noexcept {
  const FxHeader& h = *Header(state);
  const EffectKind kind = static_cast<EffectKind>(h.kind);
  ProcessOne(kind, GetEffectInfo(kind), state, in, out, channels, frames, params, sidechain);
}

void EffectProcessBatch(EffectKind kind, const EffectBatchItem* items,
                        std::uint32_t count) noexcept {
  const EffectInfo& info = GetEffectInfo(kind);
  // One switch per batch; the per-item loop calls the kind directly.
  switch (kind) {
#define SND_FX_BATCH(Kind)                                                                     \
  case EffectKind::Kind:                                                                       \
    for (std::uint32_t i = 0; i < count; ++i) {                                                \
      const EffectBatchItem& it = items[i];                                                    \
      ProcessOne(EffectKind::Kind, info, it.state, it.in, it.out, it.channels, it.frames,      \
                 it.params, it.sidechain);                                                     \
    }                                                                                          \
    break;
    SND_FX_BATCH(Reverb)
    SND_FX_BATCH(Echo)
    SND_FX_BATCH(Delay)
    SND_FX_BATCH(Chorus)
    SND_FX_BATCH(Flanger)
    SND_FX_BATCH(Distortion)
    SND_FX_BATCH(Compressor)
    SND_FX_BATCH(Limiter)
    SND_FX_BATCH(ParamEq)
    SND_FX_BATCH(Lowpass)
    SND_FX_BATCH(Highpass)
    SND_FX_BATCH(PitchShifter)
    SND_FX_BATCH(Normalize)
    SND_FX_BATCH(Surround)
    SND_FX_BATCH(Convolution)
#undef SND_FX_BATCH
    case EffectKind::Count: break;
  }
}

bool EffectIsTailDone(const void* state) noexcept {
  const FxHeader& h = *Header(state);
  return h.silentFrames >= h.tailFrames;
}

void EffectReset(void* state) noexcept { ReinitKeepingParams(state); }

EffectKind EffectStateKind(const void* state) noexcept {
  return static_cast<EffectKind>(Header(state)->kind);
}

float EffectParamValue(const void* state, std::uint32_t index) noexcept {
  const FxHeader& h = *Header(state);
  return index < h.paramCount ? h.cur[index] : 0.0f;
}

std::uint32_t EffectLatencyFrames(const void* state) noexcept { return Header(state)->latency; }

void ProcessEffectChain(const EffectSlot* slots, std::uint32_t count, float* const* buffers,
                        std::uint32_t channels, std::uint32_t frames) noexcept {
  for (std::uint32_t i = 0; i < count; ++i) {
    const EffectSlot& s = slots[i];
    if (s.bypass != 0 || s.state == nullptr) continue;
    if (s.plugin != nullptr) {
      if (s.plugin->process == nullptr) continue;
      snd_param_block empty{nullptr, nullptr, 0u, 0u, nullptr};
      s.plugin->process(s.plugin->user, s.state, buffers, buffers, channels, frames,
                        s.params != nullptr ? s.params : &empty);
    } else {
      EffectProcess(s.state, buffers, buffers, channels, frames, s.params, s.sidechain);
    }
  }
}

bool EffectSlotIsTailDone(const EffectSlot& slot) noexcept {
  if (slot.state == nullptr) return true;
  if (slot.plugin != nullptr) {
    return slot.plugin->is_tail_done == nullptr ||
           slot.plugin->is_tail_done(slot.plugin->user, slot.state) != 0;
  }
  return EffectIsTailDone(slot.state);
}

std::uint32_t PluginStateBytes(const snd_dsp_effect& plugin, const EffectDesc& desc) noexcept {
  if (plugin.state_bytes == nullptr) return 0;
  return RoundUp64(plugin.state_bytes(plugin.user, &desc));
}

std::uint32_t EffectInstanceBytes(const EffectDesc& desc, std::uint32_t sampleRate,
                                  const snd_dsp_effect* plugins,
                                  std::uint32_t pluginCount) noexcept {
  // Plugins with the id of a standard effect take precedence (D-31: registration overrides).
  for (std::uint32_t i = 0; plugins != nullptr && i < pluginCount; ++i) {
    if (plugins[i].effect_type == desc.effect_type) {
      // D-48: the plugin sees the rate in the desc.
      if (desc.sample_rate != 0u && desc.sample_rate != sampleRate) return 0;
      EffectDesc withRate = desc;
      withRate.sample_rate = sampleRate;
      return PluginStateBytes(plugins[i], withRate);
    }
  }
  return EffectStateBytes(desc, sampleRate);
}

bank_format::DspPoolNeed ComputeDspPoolNeed(const EffectDesc* descs, std::uint32_t count,
                                            std::uint32_t sampleRate,
                                            const snd_dsp_effect* plugins,
                                            std::uint32_t pluginCount,
                                            std::uint32_t* unknown) noexcept {
  bank_format::DspPoolNeed need{0u, 0u, 0u};
  std::uint32_t bad = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t b = EffectInstanceBytes(descs[i], sampleRate, plugins, pluginCount);
    if (b == 0) {
      ++bad;
      continue;
    }
    need.instanceBytes += b;
    ++need.instanceCount;
  }
  if (unknown != nullptr) *unknown = bad;
  return need;
}

}  // namespace snd::dsp
