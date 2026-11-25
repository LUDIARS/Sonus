// Filter-type effects (parametric EQ, lowpass, highpass), distortion and surround.
#include <cmath>
#include <cstring>

#include "fx_common.hpp"
#include "snd/dsp/filter.hpp"

namespace snd::dsp::fx {

namespace {

using detail::At;

constexpr std::uint32_t kSub = 32;  // coefficient update interval
constexpr std::uint32_t kBiquadTail = 512;

// Runs one biquad over the block; coefficients designed every kSub frames from
// interpolated parameters (exactly constant when the parameters do not ramp).
template <class DesignFn>
void RunBiquad(BiquadState& st, BiquadCoefs& cached, bool ramps, DesignFn design,
               const float* const* in, float* const* out, std::uint32_t channels,
               std::uint32_t frames) noexcept {
  if (!ramps) {
    cached = design(1.0f);
    BiquadProcess(st, cached, cached, in, out, channels, frames);
    return;
  }
  const float* inp[kMaxChannels];
  float* outp[kMaxChannels];
  BiquadCoefs from = design(0.0f);
  for (std::uint32_t s = 0; s < frames; s += kSub) {
    const std::uint32_t n = frames - s < kSub ? frames - s : kSub;
    const BiquadCoefs to = design(static_cast<float>(s + n) / static_cast<float>(frames));
    for (std::uint32_t c = 0; c < channels; ++c) {
      inp[c] = in[c] + s;
      outp[c] = out[c] + s;
    }
    BiquadProcess(st, from, to, inp, outp, channels, n);
    from = to;
  }
  cached = from;
}

float StatePeak(const BiquadState& st, std::uint32_t channels) noexcept {
  float m = 0.0f;
  for (std::uint32_t c = 0; c < channels; ++c) {
    m = detail::Max(m, detail::Max(std::fabs(st.z1[c]), std::fabs(st.z2[c])));
  }
  return m;
}

void CopyThrough(const FxIo& io) noexcept {
  for (std::uint32_t c = 0; c < io.channels; ++c) Copy(io.out[c], io.in[c], io.frames);
}

}  // namespace

// ---------------------------------------------------------------------------
// Parametric EQ (4 bands)
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint32_t kBands = 4;
struct ParamEqState {
  FxHeader h;
  BiquadState band[kBands];
  BiquadCoefs coefs[kBands];
};

bool BandType(float v, BiquadType* t) noexcept {
  switch (static_cast<int>(v)) {
    case 1: *t = BiquadType::Peaking; return true;
    case 2: *t = BiquadType::LowShelf; return true;
    case 3: *t = BiquadType::HighShelf; return true;
    case 4: *t = BiquadType::Lowpass; return true;
    case 5: *t = BiquadType::Highpass; return true;
    case 6: *t = BiquadType::Bandpass; return true;
    case 7: *t = BiquadType::Notch; return true;
    case 8: *t = BiquadType::Allpass; return true;  // D-135 (stage 3): appended
    default: return false;
  }
}
}  // namespace

std::size_t ParamEqLayout(void*, const FxConfig&) noexcept { return sizeof(ParamEqState); }
void ParamEqReset(void* state) noexcept {
  auto* s = static_cast<ParamEqState*>(state);
  for (auto& b : s->band) BiquadReset(b);
}
void ParamEqInit(void* state, const FxConfig&) noexcept {
  ParamEqReset(state);
  static_cast<ParamEqState*>(state)->h.tailFrames = kBiquadTail;
}

void ParamEqProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<ParamEqState*>(state);
  const float sr = static_cast<float>(s.h.sampleRate);
  bool any = false;
  float peak = 0.0f;
  for (std::uint32_t b = 0; b < kBands; ++b) {
    BiquadType type;
    if (!BandType(p.e[4 * b], &type)) continue;
    const std::uint32_t k = 4 * b;
    const bool ramps = p.Ramps(k + 1) || p.Ramps(k + 2) || p.Ramps(k + 3);
    auto design = [&](float t) {
      return DesignBiquad(type, p.At(k + 1, t), p.At(k + 2, t), p.At(k + 3, t), sr);
    };
    RunBiquad(s.band[b], s.coefs[b], ramps, design, any ? io.out : io.in, io.out, io.channels,
              io.frames);
    any = true;
    BiquadFlush(s.band[b], io.channels);
    peak = detail::Max(peak, StatePeak(s.band[b], io.channels));
  }
  if (!any) CopyThrough(io);
  peak = detail::Max(peak, detail::BlockPeak(io.out, io.channels, io.frames));
  TailUpdate(s.h, peak, io.frames);
  if (TailShouldClear(s.h)) ParamEqReset(state);
}

// ---------------------------------------------------------------------------
// Lowpass / highpass
// ---------------------------------------------------------------------------
namespace {
struct SimpleFilterState {
  FxHeader h;
  BiquadState st;
  BiquadCoefs coefs;
};

void SimpleFilterProcess(void* state, const FxIo& io, const FxParams& p, BiquadType type) noexcept {
  auto& s = *static_cast<SimpleFilterState*>(state);
  const float sr = static_cast<float>(s.h.sampleRate);
  auto design = [&](float t) { return DesignBiquad(type, p.At(0, t), p.At(1, t), 0.0f, sr); };
  RunBiquad(s.st, s.coefs, p.Ramps(0) || p.Ramps(1), design, io.in, io.out, io.channels, io.frames);
  BiquadFlush(s.st, io.channels);
  const float peak = detail::Max(StatePeak(s.st, io.channels),
                                 detail::BlockPeak(io.out, io.channels, io.frames));
  TailUpdate(s.h, peak, io.frames);
  if (TailShouldClear(s.h)) BiquadReset(s.st);
}
}  // namespace

std::size_t LowpassLayout(void*, const FxConfig&) noexcept { return sizeof(SimpleFilterState); }
void LowpassReset(void* state) noexcept { BiquadReset(static_cast<SimpleFilterState*>(state)->st); }
void LowpassInit(void* state, const FxConfig&) noexcept {
  LowpassReset(state);
  static_cast<SimpleFilterState*>(state)->h.tailFrames = kBiquadTail;
}
void LowpassProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  SimpleFilterProcess(state, io, p, BiquadType::Lowpass);
}

std::size_t HighpassLayout(void*, const FxConfig&) noexcept { return sizeof(SimpleFilterState); }
void HighpassReset(void* state) noexcept { BiquadReset(static_cast<SimpleFilterState*>(state)->st); }
void HighpassInit(void* state, const FxConfig&) noexcept {
  HighpassReset(state);
  static_cast<SimpleFilterState*>(state)->h.tailFrames = kBiquadTail;
}
void HighpassProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  SimpleFilterProcess(state, io, p, BiquadType::Highpass);
}

// ---------------------------------------------------------------------------
// Distortion: drive gain (0..40 dB) -> rational soft clipper -> tone lowpass.
// ---------------------------------------------------------------------------
namespace {
struct DistortionState {
  FxHeader h;
  float lp[kMaxChannels];
};
inline float SoftClip(float v) noexcept {
  if (v > 3.0f) return 1.0f;
  if (v < -3.0f) return -1.0f;
  const float v2 = v * v;
  return v * (27.0f + v2) / (27.0f + 9.0f * v2);
}
}  // namespace

std::size_t DistortionLayout(void*, const FxConfig&) noexcept { return sizeof(DistortionState); }
void DistortionReset(void* state) noexcept {
  std::memset(static_cast<DistortionState*>(state)->lp, 0, sizeof(float) * kMaxChannels);
}
void DistortionInit(void* state, const FxConfig&) noexcept {
  DistortionReset(state);
  static_cast<DistortionState*>(state)->h.tailFrames = 64;
}

void DistortionProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<DistortionState*>(state);
  const float sr = static_cast<float>(s.h.sampleRate);
  const std::uint32_t n = io.frames;
  const float inv = 1.0f / static_cast<float>(n);
  auto toneCoef = [sr](float hz) {
    return hz >= 19999.0f ? 0.0f : std::exp(-2.0f * detail::kPiF * hz / sr);
  };
  // Parameters ramp linearly (D-26); the derived gains / coefficient are
  // computed per frame from the interpolated parameter while it ramps.
  const bool rDrive = p.Ramps(0), rOut = p.Ramps(2), rTone = p.Ramps(3);
  const float dC = DbToGain(p.s[0] * 40.0f), oC = DbToGain(p.s[2]), aC = toneCoef(p.s[3]);
  float peak = 0.0f;
  for (std::uint32_t c = 0; c < io.channels; ++c) {
    float lp = s.lp[c];
    for (std::uint32_t i = 0; i < n; ++i) {
      const float t = static_cast<float>(i) * inv;
      const float x = io.in[c][i];
      const float drive = rDrive ? DbToGain(p.At(0, t) * 40.0f) : dC;
      const float a = rTone ? toneCoef(p.At(3, t)) : aC;
      const float og = rOut ? DbToGain(p.At(2, t)) : oC;
      const float v = SoftClip(x * drive);
      lp = detail::FlushState(v + (lp - v) * a);
      const float mix = p.At(1, t);
      const float y = ((1.0f - mix) * x + mix * lp) * og;
      io.out[c][i] = y;
      peak = detail::Max(peak, std::fabs(y));
    }
    s.lp[c] = detail::Flush(lp);
  }
  TailUpdate(s.h, peak, n);
  if (TailShouldClear(s.h)) DistortionReset(state);
}

// ---------------------------------------------------------------------------
// Surround: stereo width (M/S) on channels 0/1; for 6 / 8 channel layouts the
// rears get delayed, low-passed L - 0.5 R / R - 0.5 L, and the centre (L+R)/2.
// ---------------------------------------------------------------------------
namespace {
struct SurroundState {
  FxHeader h;
  std::uint32_t ringOff, size, write, pad;
  float lp[2];
};
}  // namespace

std::size_t SurroundLayout(void* state, const FxConfig& cfg) noexcept {
  detail::Carver c(state, sizeof(SurroundState));
  const std::uint32_t size = MsToFrames(50.0f, cfg.sizingRate) + 4u;
  const std::uint32_t off = c.Take<float>(2u * size);
  if (state != nullptr) {
    auto* s = static_cast<SurroundState*>(state);
    s->ringOff = off;
    s->size = size;
  }
  return c.Size();
}
void SurroundReset(void* state) noexcept {
  auto* s = static_cast<SurroundState*>(state);
  std::memset(At<float>(state, s->ringOff), 0, 2u * s->size * sizeof(float));
  s->write = 0;
  s->lp[0] = s->lp[1] = 0.0f;
}
void SurroundInit(void* state, const FxConfig&) noexcept {
  SurroundReset(state);
  auto* s = static_cast<SurroundState*>(state);
  s->h.tailFrames = s->size;
}

void SurroundProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<SurroundState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels;
  if (chs < 2) {
    CopyThrough(io);
    TailUpdate(s.h, 0.0f, n);
    return;
  }
  const float sr = static_cast<float>(s.h.sampleRate);
  const float inv = 1.0f / static_cast<float>(n);
  float* ringL = At<float>(state, s.ringOff);
  float* ringR = ringL + s.size;
  const float maxD = static_cast<float>(s.size - 3u);
  const bool rears = chs >= 6;
  auto lpCoef = [sr](float hz) { return std::exp(-2.0f * detail::kPiF * hz / sr); };
  const float a0 = lpCoef(p.s[3]), a1 = lpCoef(p.e[3]);
  const float r0 = DbToGain(p.s[1]), r1 = DbToGain(p.e[1]);
  const float c0 = p.s[4] <= -60.0f ? 0.0f : DbToGain(p.s[4]);
  const float c1 = p.e[4] <= -60.0f ? 0.0f : DbToGain(p.e[4]);
  float peak = 0.0f;
  std::uint32_t w = s.write;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float l = io.in[0][i], r = io.in[1][i];
    const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * p.At(0, t);
    float extra[kMaxChannels];
    for (std::uint32_t c = 2; c < chs; ++c) extra[c] = io.in[c][i];
    if (rears) {
      const float a = a0 + (a1 - a0) * t;
      const float sl = l - 0.5f * r, sr2 = r - 0.5f * l;
      s.lp[0] = detail::FlushState(sl + (s.lp[0] - sl) * a);
      s.lp[1] = detail::FlushState(sr2 + (s.lp[1] - sr2) * a);
      const float delay = detail::Clamp(p.At(2, t) * 0.001f * sr, 1.0f, maxD) - 1.0f;
      const float dl = detail::ReadFrac(ringL, s.size, w, delay);
      const float dr = detail::ReadFrac(ringR, s.size, w, delay);
      ringL[w] = s.lp[0];
      ringR[w] = s.lp[1];
      const float g = r0 + (r1 - r0) * t;
      extra[2] += mid * (c0 + (c1 - c0) * t);
      extra[4] += g * dl;
      extra[5] += g * dr;
      if (chs >= 8) {
        extra[6] += 0.70710678f * g * dl;
        extra[7] += 0.70710678f * g * dr;
      }
      peak = detail::Max(peak, detail::Max(std::fabs(s.lp[0]), std::fabs(s.lp[1])));
    }
    io.out[0][i] = mid + side;
    io.out[1][i] = mid - side;
    for (std::uint32_t c = 2; c < chs; ++c) io.out[c][i] = extra[c];
    w = w + 1u == s.size ? 0u : w + 1u;
  }
  s.write = w;
  TailUpdate(s.h, peak, n);
  if (TailShouldClear(s.h)) {
    SurroundReset(state);
  } else {
    s.lp[0] = detail::Flush(s.lp[0]);
    s.lp[1] = detail::Flush(s.lp[1]);
  }
}

}  // namespace snd::dsp::fx
