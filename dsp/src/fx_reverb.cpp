// Algorithmic reverb: I3DL2 parameter set, 8-line feedback delay network.
//
//   in (mean of channels) * Room -> RoomLF shelf -> RoomHF lowpass -> pre-delay line
//   early reflections: 8 taps from ReflectionsDelay, decorrelated per channel
//   late: tap at ReflectionsDelay + ReverbDelay -> 4 allpass diffusers -> FDN
//   FDN: 8 lines, Householder feedback, per-line gain for DecayTime and a
//        one-pole damping for DecayHFRatio at HFReference; Density scales the
//        line lengths (fractional reads).
//   out_c = dry * in_c + Reflections * ER_c + Reverb * late_c
// Output levels are not calibrated against a reference implementation
// ("近似", design 7).
#include <cmath>
#include <cstring>

#include "fx_common.hpp"

namespace snd::dsp::fx {

namespace {

using detail::At;
using detail::ReadFrac;

constexpr std::uint32_t kLines = 8;
constexpr std::uint32_t kAllpass = 4;
constexpr std::uint32_t kTaps = 8;
constexpr float kLineBase[kLines] = {1031, 1153, 1277, 1361, 1499, 1607, 1741, 1877};  // @48 kHz
constexpr float kApBase[kAllpass] = {225, 341, 441, 556};
constexpr float kTapMs[kTaps] = {0.0f, 2.3f, 4.1f, 6.7f, 9.2f, 11.9f, 14.3f, 17.1f};
constexpr float kTapGain[kTaps] = {1.0f, 0.86f, 0.78f, 0.66f, 0.58f, 0.49f, 0.42f, 0.35f};
constexpr float kPreMaxSeconds = 0.3f + 0.1f + 0.02f;
constexpr float kInvSqrt8 = 0.35355339f;

struct ReverbState {
  FxHeader h;
  std::uint32_t preOff, preSize, preW, scratchOff;
  std::uint32_t lineOff[kLines], lineSize[kLines], lineW[kLines];
  std::uint32_t apOff[kAllpass], apSize[kAllpass], apW[kAllpass];
  float lineLp[kLines];
  float inLf, inHf;
  float scale;  // runtime rate / 48000
  float pad;
};

// DryLevel uses -10000 mB as "off".
float MbToLinSafe(float mb) noexcept { return mb <= -10000.0f ? 0.0f : detail::MbToLin(mb); }

inline std::uint32_t Next(std::uint32_t w, std::uint32_t size) noexcept {
  return w + 1u == size ? 0u : w + 1u;
}

float Hadamard(std::uint32_t r, std::uint32_t c) noexcept {
  return (__builtin_popcount(r & c) & 1) ? -1.0f : 1.0f;
}

// One-pole lowpass coefficient a (y += (1-a)(x-y)) with magnitude m at w.
float OnePoleForMagnitude(float m, float w) noexcept {
  if (!(m < 0.999f)) return 0.0f;
  if (m < 0.001f) m = 0.001f;
  const float m2 = m * m, cw = std::cos(w);
  const float A = 1.0f - m2;
  const float B = 1.0f - m2 * cw;
  const float disc = B * B - A * A;
  const float a = (B - std::sqrt(disc > 0.0f ? disc : 0.0f)) / A;
  return detail::Clamp(a, 0.0f, 0.9999f);
}

struct Coefs {
  float room, roomHfA, lfGain, lfA, dry, refl, rev, diff, density;
  float g[kLines], damp[kLines], len[kLines];
  float erDelay, lateDelay;  // frames
};

Coefs MakeCoefs(const ReverbState& s, const float* v) noexcept {
  Coefs k;
  const float sr = static_cast<float>(s.h.sampleRate);
  k.dry = MbToLinSafe(v[0]);
  k.room = detail::MbToLin(v[1]);
  const float wHf = 2.0f * detail::kPiF * detail::Min(v[12], 0.45f * sr) / sr;
  k.roomHfA = OnePoleForMagnitude(detail::MbToLin(v[2]), wHf);
  k.lfGain = detail::MbToLin(v[3]);
  k.lfA = std::exp(-2.0f * detail::kPiF * v[13] / sr);
  const float t60 = v[4];
  const float hfRatio = detail::Min(v[5], 1.0f);
  k.refl = detail::MbToLin(v[6]);
  k.rev = detail::MbToLin(v[8]);
  k.diff = 0.7f * v[10] * 0.01f;
  k.density = 0.5f + 0.5f * v[11] * 0.01f;
  const float maxPre = static_cast<float>(s.preSize - 3u);
  k.erDelay = detail::Clamp(v[7] * sr, 1.0f, maxPre);
  k.lateDelay = detail::Clamp((v[7] + v[9]) * sr, 1.0f, maxPre);
  for (std::uint32_t l = 0; l < kLines; ++l) {
    const float len = detail::Clamp(kLineBase[l] * s.scale * k.density, 4.0f,
                                    static_cast<float>(s.lineSize[l] - 3u));
    k.len[l] = len;
    k.g[l] = std::pow(10.0f, -3.0f * len / (t60 * sr));
    const float gHf = std::pow(10.0f, -3.0f * len / (t60 * hfRatio * sr));
    k.damp[l] = OnePoleForMagnitude(gHf / k.g[l], wHf);
  }
  return k;
}

}  // namespace

std::size_t ReverbLayout(void* state, const FxConfig& cfg) noexcept {
  detail::Carver c(state, sizeof(ReverbState));
  const float scale = static_cast<float>(cfg.sizingRate) / 48000.0f;
  auto* s = static_cast<ReverbState*>(state);
  const std::uint32_t preSize =
      static_cast<std::uint32_t>(kPreMaxSeconds * static_cast<float>(cfg.sizingRate)) + 4u;
  const std::uint32_t preOff = c.Take<float>(preSize);
  std::uint32_t lineOff[kLines], lineSize[kLines], apOff[kAllpass], apSize[kAllpass];
  for (std::uint32_t l = 0; l < kLines; ++l) {
    lineSize[l] = static_cast<std::uint32_t>(kLineBase[l] * scale) + 4u;
    lineOff[l] = c.Take<float>(lineSize[l]);
  }
  for (std::uint32_t a = 0; a < kAllpass; ++a) {
    apSize[a] = static_cast<std::uint32_t>(kApBase[a] * scale) + 1u;
    apOff[a] = c.Take<float>(apSize[a]);
  }
  const std::uint32_t scratch = c.Take<float>(cfg.maxFrames);
  if (s != nullptr) {
    s->preOff = preOff;
    s->preSize = preSize;
    s->scratchOff = scratch;
    for (std::uint32_t l = 0; l < kLines; ++l) {
      s->lineOff[l] = lineOff[l];
      s->lineSize[l] = lineSize[l];
    }
    for (std::uint32_t a = 0; a < kAllpass; ++a) {
      s->apOff[a] = apOff[a];
      s->apSize[a] = apSize[a];
    }
  }
  return c.Size();
}

void ReverbReset(void* state) noexcept {
  auto* s = static_cast<ReverbState*>(state);
  std::memset(At<float>(state, s->preOff), 0, s->preSize * sizeof(float));
  for (std::uint32_t l = 0; l < kLines; ++l)
    std::memset(At<float>(state, s->lineOff[l]), 0, s->lineSize[l] * sizeof(float));
  for (std::uint32_t a = 0; a < kAllpass; ++a)
    std::memset(At<float>(state, s->apOff[a]), 0, s->apSize[a] * sizeof(float));
  std::memset(s->lineLp, 0, sizeof(s->lineLp));
  s->inLf = s->inHf = 0.0f;
  s->preW = 0;
  std::memset(s->lineW, 0, sizeof(s->lineW));
  std::memset(s->apW, 0, sizeof(s->apW));
}

void ReverbInit(void* state, const FxConfig&) noexcept {
  auto* s = static_cast<ReverbState*>(state);
  s->scale = static_cast<float>(s->h.sampleRate) / 48000.0f;
  std::uint32_t tail = s->preSize;
  std::uint32_t longest = 0;
  for (std::uint32_t l = 0; l < kLines; ++l) longest = detail::Max(longest, s->lineSize[l]);
  for (std::uint32_t a = 0; a < kAllpass; ++a) tail += s->apSize[a];
  s->h.tailFrames = tail + longest;
}

void ReverbProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<ReverbState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels;
  const Coefs k0 = MakeCoefs(s, p.s);
  const Coefs k1 = MakeCoefs(s, p.e);
  float* mono = At<float>(state, s.scratchOff);
  const float invCh = 1.0f / static_cast<float>(chs);
  for (std::uint32_t i = 0; i < n; ++i) {
    float m = 0.0f;
    for (std::uint32_t c = 0; c < chs; ++c) m += io.in[c][i];
    mono[i] = m * invCh;
  }
  float* pre = At<float>(state, s.preOff);
  float* lines[kLines];
  for (std::uint32_t l = 0; l < kLines; ++l) lines[l] = At<float>(state, s.lineOff[l]);
  float* aps[kAllpass];
  for (std::uint32_t a = 0; a < kAllpass; ++a) aps[a] = At<float>(state, s.apOff[a]);
  const float sr = static_cast<float>(s.h.sampleRate);
  float tapOff[kTaps];
  for (std::uint32_t j = 0; j < kTaps; ++j) tapOff[j] = kTapMs[j] * 0.001f * sr;
  const float maxPre = static_cast<float>(s.preSize - 3u);

  const float inv = 1.0f / static_cast<float>(n);
  float peak = 0.0f;
  float dry[kMaxChannels];
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    auto L = [t](float a, float b) { return a + (b - a) * t; };
    // Input conditioning.
    float x = mono[i] * L(k0.room, k1.room);
    s.inLf = detail::FlushState(x + (s.inLf - x) * L(k0.lfA, k1.lfA));
    x = x - (1.0f - L(k0.lfGain, k1.lfGain)) * s.inLf;
    s.inHf = detail::FlushState(x + (s.inHf - x) * L(k0.roomHfA, k1.roomHfA));
    x = s.inHf;
    const std::uint32_t pw = s.preW;
    // Early reflections (read before writing x: delay 0 = previous sample).
    float taps[kTaps];
    const float er = L(k0.erDelay, k1.erDelay);
    for (std::uint32_t j = 0; j < kTaps; ++j) {
      taps[j] = kTapGain[j] * ReadFrac(pre, s.preSize, pw, detail::Min(er + tapOff[j], maxPre) - 1.0f);
    }
    float late = ReadFrac(pre, s.preSize, pw, L(k0.lateDelay, k1.lateDelay) - 1.0f);
    pre[pw] = x;
    s.preW = Next(pw, s.preSize);
    // Diffusion.
    const float g = L(k0.diff, k1.diff);
    for (std::uint32_t a = 0; a < kAllpass; ++a) {
      const std::uint32_t ai = s.apW[a];
      const float d = aps[a][ai];
      const float y = d - g * late;
      aps[a][ai] = detail::FlushState(late + g * y);
      s.apW[a] = Next(ai, s.apSize[a]);
      late = y;
    }
    // FDN.
    float y[kLines], v[kLines];
    float sum = 0.0f;
    for (std::uint32_t l = 0; l < kLines; ++l) {
      y[l] = ReadFrac(lines[l], s.lineSize[l], s.lineW[l], L(k0.len[l], k1.len[l]) - 1.0f);
      s.lineLp[l] = detail::FlushState(y[l] + (s.lineLp[l] - y[l]) * L(k0.damp[l], k1.damp[l]));
      v[l] = L(k0.g[l], k1.g[l]) * s.lineLp[l];
      sum += v[l];
    }
    sum *= 0.25f;  // Householder 2/N
    for (std::uint32_t l = 0; l < kLines; ++l) {
      const float in = detail::FlushState(v[l] - sum + late);
      lines[l][s.lineW[l]] = in;
      s.lineW[l] = Next(s.lineW[l], s.lineSize[l]);
      const float a = std::fabs(in);
      peak = a > peak ? a : peak;
    }
    {
      const float a = std::fabs(x);
      peak = a > peak ? a : peak;
    }
    // Outputs.
    const float dg = L(k0.dry, k1.dry), rg = L(k0.refl, k1.refl), lg = L(k0.rev, k1.rev);
    for (std::uint32_t c = 0; c < chs; ++c) dry[c] = io.in[c][i];
    for (std::uint32_t c = 0; c < chs; ++c) {
      float e = 0.0f, l = 0.0f;
      for (std::uint32_t j = 0; j < kTaps; ++j) e += Hadamard(c, j) * taps[j];
      for (std::uint32_t q = 0; q < kLines; ++q) l += Hadamard(c + 1u, q) * y[q];
      io.out[c][i] = dg * dry[c] + (rg * e + lg * l) * kInvSqrt8;
    }
  }
  TailUpdate(s.h, peak, n);
  if (TailShouldClear(s.h)) {
    ReverbReset(state);
  } else {
    for (std::uint32_t l = 0; l < kLines; ++l) s.lineLp[l] = detail::Flush(s.lineLp[l]);
    s.inLf = detail::Flush(s.inLf);
    s.inHf = detail::Flush(s.inHf);
  }
}

}  // namespace snd::dsp::fx
