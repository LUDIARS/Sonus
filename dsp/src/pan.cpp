#include "snd/dsp/pan.hpp"

#include <cmath>
#include <cstring>

#include "detail.hpp"

namespace snd::dsp {

namespace {

constexpr float kDeg = detail::kPiF / 180.0f;
constexpr float kInvSqrt2 = 0.70710678118f;
constexpr float kLfe = 1000.0f;  // marker: not a positioned speaker

struct LayoutDesc {
  std::uint32_t channels;
  float azimuthDeg[kMaxChannels];
  // Positioned speakers sorted by azimuth (-180..180): channel indices.
  std::uint32_t ringCount;
  std::uint32_t ring[kMaxChannels];
};

constexpr LayoutDesc kLayouts[] = {
    {1, {0}, 1, {0}},
    {2, {-30, 30}, 2, {0, 1}},
    {4, {-45, 45, -135, 135}, 4, {2, 0, 1, 3}},
    {6, {-30, 30, 0, kLfe, -110, 110}, 5, {4, 0, 2, 1, 5}},
    {8, {-30, 30, 0, kLfe, -150, 150, -90, 90}, 7, {4, 6, 0, 2, 1, 7, 5}},
};

const LayoutDesc& Desc(SpeakerLayout l) noexcept {
  const std::uint32_t i = static_cast<std::uint32_t>(l);
  return kLayouts[i < 5 ? i : 1];
}

float WrapPi(float a) noexcept {
  if (!(a == a)) return 0.0f;
  a = std::fmod(a + detail::kPiF, 2.0f * detail::kPiF);
  if (a < 0.0f) a += 2.0f * detail::kPiF;
  return a - detail::kPiF;
}

// Point-source gains into `g` (all channels of the layout, LFE 0).
void PointGains(SpeakerLayout layout, float az, float* g) noexcept {
  const LayoutDesc& d = Desc(layout);
  for (std::uint32_t c = 0; c < d.channels; ++c) g[c] = 0.0f;
  if (layout == SpeakerLayout::Mono) {
    g[0] = 1.0f;
    return;
  }
  az = WrapPi(az);
  if (layout == SpeakerLayout::Stereo) {
    StereoPanGains(std::sin(az), PanLaw::ConstantPower, &g[0], &g[1]);
    return;
  }
  const float azDeg = az / kDeg;
  // Find the ring pair (a, b) with a <= az < b, wrapping from the last to the first.
  const std::uint32_t n = d.ringCount;
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::uint32_t ca = d.ring[k];
    const std::uint32_t cb = d.ring[(k + 1) % n];
    float a = d.azimuthDeg[ca];
    float b = d.azimuthDeg[cb];
    float x = azDeg;
    if (k + 1 == n) {  // wrap pair across +-180
      b += 360.0f;
      if (x < a) x += 360.0f;
    }
    if (x >= a && x <= b) {
      const float t = detail::Clamp((x - a) / (b - a), 0.0f, 1.0f);
      g[ca] = detail::Max(0.0f, std::cos(t * detail::kPiF * 0.5f));  // cos(pi/2) < 0 in float
      g[cb] = detail::Max(0.0f, std::sin(t * detail::kPiF * 0.5f));
      return;
    }
  }
  g[d.ring[0]] = 1.0f;  // not reached
}

void Identity(PanMatrix* m, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) m->gain[i][i] = 1.0f;
}

// Direct downmix steps; returns false if no direct rule exists.
bool Direct(SpeakerLayout from, SpeakerLayout to, PanMatrix* m, bool lfe) noexcept {
  using L = SpeakerLayout;
  ClearPanMatrix(m);
  const float h = kInvSqrt2;
  const float lfeGain = lfe ? h : 0.0f;
  if (from == to) {
    Identity(m, LayoutChannels(from));
    return true;
  }
  if (to == L::Mono) {
    switch (from) {
      case L::Stereo: m->gain[0][0] = h; m->gain[1][0] = h; return true;
      case L::Quad:
        m->gain[0][0] = h; m->gain[1][0] = h; m->gain[2][0] = 0.5f; m->gain[3][0] = 0.5f;
        return true;
      case L::Surround51:
        m->gain[0][0] = h; m->gain[1][0] = h; m->gain[2][0] = 1.0f; m->gain[3][0] = lfeGain;
        m->gain[4][0] = 0.5f; m->gain[5][0] = 0.5f;
        return true;
      case L::Surround71:
        m->gain[0][0] = h; m->gain[1][0] = h; m->gain[2][0] = 1.0f; m->gain[3][0] = lfeGain;
        for (std::uint32_t c = 4; c < 8; ++c) m->gain[c][0] = 0.5f * h;
        return true;
      default: return false;
    }
  }
  if (from == L::Mono) {
    switch (to) {
      case L::Stereo:
      case L::Quad: m->gain[0][0] = h; m->gain[0][1] = h; return true;
      case L::Surround51:
      case L::Surround71: m->gain[0][2] = 1.0f; return true;
      default: return false;
    }
  }
  if (from == L::Stereo) {  // up: fronts only
    m->gain[0][0] = 1.0f;
    m->gain[1][1] = 1.0f;
    return true;
  }
  if (from == L::Quad) {
    switch (to) {
      case L::Stereo:
        m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f; m->gain[2][0] = h; m->gain[3][1] = h;
        return true;
      case L::Surround51:
        m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f; m->gain[2][4] = 1.0f; m->gain[3][5] = 1.0f;
        return true;
      case L::Surround71:
        m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f; m->gain[2][4] = 1.0f; m->gain[3][5] = 1.0f;
        return true;
      default: return false;
    }
  }
  if (from == L::Surround51) {
    switch (to) {
      case L::Stereo:
        m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f;
        m->gain[2][0] = h; m->gain[2][1] = h;
        m->gain[3][0] = lfeGain; m->gain[3][1] = lfeGain;
        m->gain[4][0] = h; m->gain[5][1] = h;
        return true;
      case L::Quad:
        m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f;
        m->gain[2][0] = h; m->gain[2][1] = h;
        m->gain[3][0] = lfeGain; m->gain[3][1] = lfeGain;
        m->gain[4][2] = 1.0f; m->gain[5][3] = 1.0f;
        return true;
      case L::Surround71:
        m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f; m->gain[2][2] = 1.0f; m->gain[3][3] = 1.0f;
        m->gain[4][6] = 1.0f; m->gain[5][7] = 1.0f;
        return true;
      default: return false;
    }
  }
  if (from == L::Surround71 && to == L::Surround51) {
    m->gain[0][0] = 1.0f; m->gain[1][1] = 1.0f; m->gain[2][2] = 1.0f; m->gain[3][3] = 1.0f;
    m->gain[4][4] = h; m->gain[6][4] = h;
    m->gain[5][5] = h; m->gain[7][5] = h;
    return true;
  }
  return false;
}

}  // namespace

std::uint32_t LayoutChannels(SpeakerLayout layout) noexcept { return Desc(layout).channels; }

bool LayoutForChannels(std::uint32_t channels, SpeakerLayout* out) noexcept {
  switch (channels) {
    case 1: *out = SpeakerLayout::Mono; return true;
    case 2: *out = SpeakerLayout::Stereo; return true;
    case 4: *out = SpeakerLayout::Quad; return true;
    case 6: *out = SpeakerLayout::Surround51; return true;
    case 8: *out = SpeakerLayout::Surround71; return true;
    default: return false;
  }
}

int LayoutLfeChannel(SpeakerLayout layout) noexcept {
  return (layout == SpeakerLayout::Surround51 || layout == SpeakerLayout::Surround71) ? 3 : -1;
}

void StereoPanGains(float pan, PanLaw law, float* left, float* right) noexcept {
  if (!(pan == pan)) pan = 0.0f;
  pan = detail::Clamp(pan, -1.0f, 1.0f);
  const float theta = (pan + 1.0f) * detail::kPiF * 0.25f;
  const float cl = detail::Max(0.0f, std::cos(theta)), cr = detail::Max(0.0f, std::sin(theta));
  const float ll = 0.5f * (1.0f - pan), lr = 0.5f * (1.0f + pan);
  switch (law) {
    case PanLaw::ConstantPower: *left = cl; *right = cr; return;
    case PanLaw::Linear: *left = ll; *right = lr; return;
    case PanLaw::Compromise: *left = std::sqrt(cl * ll); *right = std::sqrt(cr * lr); return;
  }
  *left = cl;
  *right = cr;
}

void ComputePanGains(SpeakerLayout layout, float azimuth, float spread, float* gains) noexcept {
  const std::uint32_t n = LayoutChannels(layout);
  if (!(spread == spread)) spread = 0.0f;
  spread = detail::Clamp(spread, 0.0f, 1.0f);
  if (spread <= 0.0f || layout == SpeakerLayout::Mono) {
    PointGains(layout, azimuth, gains);
    return;
  }
  // Average the power of K point sources spread evenly over the arc.
  const std::uint32_t k = 1u + static_cast<std::uint32_t>(std::ceil(spread * 32.0f));
  const float width = spread * 2.0f * detail::kPiF;
  float power[kMaxChannels] = {};
  float g[kMaxChannels];
  for (std::uint32_t s = 0; s < k; ++s) {
    const float off = ((static_cast<float>(s) + 0.5f) / static_cast<float>(k) - 0.5f) * width;
    PointGains(layout, azimuth + off, g);
    for (std::uint32_t c = 0; c < n; ++c) power[c] += g[c] * g[c];
  }
  const float inv = 1.0f / static_cast<float>(k);
  for (std::uint32_t c = 0; c < n; ++c) gains[c] = std::sqrt(power[c] * inv);
}

void ClearPanMatrix(PanMatrix* m) noexcept { std::memset(m, 0, sizeof(PanMatrix)); }

void ScalePanMatrix(PanMatrix* m, float gain) noexcept {
  for (auto& row : m->gain)
    for (float& v : row) v *= gain;
}

void LerpPanMatrix(const PanMatrix& a, const PanMatrix& b, float t, PanMatrix* out) noexcept {
  for (std::uint32_t i = 0; i < kMaxChannels; ++i)
    for (std::uint32_t o = 0; o < kMaxChannels; ++o)
      out->gain[i][o] = a.gain[i][o] + (b.gain[i][o] - a.gain[i][o]) * t;
}

void MultiplyPanMatrix(const PanMatrix& a, const PanMatrix& b, PanMatrix* out) noexcept {
  PanMatrix r;
  for (std::uint32_t i = 0; i < kMaxChannels; ++i) {
    for (std::uint32_t k = 0; k < kMaxChannels; ++k) {
      float s = 0.0f;
      for (std::uint32_t j = 0; j < kMaxChannels; ++j) s += a.gain[i][j] * b.gain[j][k];
      r.gain[i][k] = s;
    }
  }
  *out = r;
}

void ComputePanMatrix(std::uint32_t inChannels, SpeakerLayout out, float azimuth, float spread,
                      PanMatrix* m) noexcept {
  ClearPanMatrix(m);
  if (inChannels == 0) return;
  if (inChannels > kMaxChannels) inChannels = kMaxChannels;
  if (!(spread == spread)) spread = 0.0f;
  spread = detail::Clamp(spread, 0.0f, 1.0f);
  if (inChannels == 1) {
    ComputePanGains(out, azimuth, spread, m->gain[0]);
    return;
  }
  // Multi-channel source: channels placed over spread * 90 degrees, each panned
  // as a point; rows scaled by 1/sqrt(n) (power preserving for uncorrelated channels).
  const float width = spread * detail::kPiF * 0.5f;
  const float scale = 1.0f / std::sqrt(static_cast<float>(inChannels));
  const std::uint32_t n = LayoutChannels(out);
  for (std::uint32_t c = 0; c < inChannels; ++c) {
    const float off = (static_cast<float>(c) / static_cast<float>(inChannels - 1) - 0.5f) * width;
    PointGains(out, azimuth + off, m->gain[c]);
    for (std::uint32_t o = 0; o < n; ++o) m->gain[c][o] *= scale;
  }
}

void ComputeDownmixMatrix(SpeakerLayout from, SpeakerLayout to, PanMatrix* m,
                          bool includeLfe) noexcept {
  if (Direct(from, to, m, includeLfe)) return;
  // 7.1 -> stereo / quad: through 5.1.
  PanMatrix a, b;
  Direct(from, SpeakerLayout::Surround51, &a, includeLfe);
  Direct(SpeakerLayout::Surround51, to, &b, includeLfe);
  MultiplyPanMatrix(a, b, m);
}

void ComputeStereoPanMatrix(std::uint32_t inChannels, SpeakerLayout out, float pan, PanLaw law,
                            PanMatrix* m) noexcept {
  ClearPanMatrix(m);
  if (inChannels == 0) return;
  if (!(pan == pan)) pan = 0.0f;
  pan = detail::Clamp(pan, -1.0f, 1.0f);
  if (out == SpeakerLayout::Mono) {
    SpeakerLayout in;
    if (!LayoutForChannels(inChannels, &in)) in = SpeakerLayout::Mono;
    if (inChannels == 1) {
      m->gain[0][0] = 1.0f;
      return;
    }
    ComputeDownmixMatrix(in, out, m);
    return;
  }
  if (inChannels == 1) {
    // Mono source: pan law between the front left/right speakers.
    StereoPanGains(pan, law, &m->gain[0][0], &m->gain[0][1]);
    return;
  }
  SpeakerLayout in;
  if (!LayoutForChannels(inChannels, &in)) {
    // Unknown layout: pairs to L/R.
    for (std::uint32_t c = 0; c < inChannels && c < kMaxChannels; ++c) m->gain[c][c & 1u] = kInvSqrt2;
  } else {
    ComputeDownmixMatrix(in, out, m);
  }
  // Balance: attenuate the side away from the pan direction.
  float gl = 1.0f, gr = 1.0f;
  const float a = std::fabs(pan);
  float att = 1.0f;
  switch (law) {
    case PanLaw::ConstantPower: att = detail::Max(0.0f, std::cos(a * detail::kPiF * 0.5f)); break;
    case PanLaw::Linear: att = 1.0f - a; break;
    case PanLaw::Compromise: att = std::sqrt(detail::Max(0.0f, std::cos(a * detail::kPiF * 0.5f)) * (1.0f - a)); break;
  }
  if (pan > 0.0f) gl = att;
  if (pan < 0.0f) gr = att;
  for (std::uint32_t i = 0; i < kMaxChannels; ++i) {
    m->gain[i][0] *= gl;
    m->gain[i][1] *= gr;
  }
}

void ApplyPanMatrix(const float* const* in, std::uint32_t inChannels, float* const* out,
                    std::uint32_t outChannels, std::uint32_t frames, const PanMatrix& from,
                    const PanMatrix& to, bool accumulate) noexcept {
  if (inChannels > kMaxChannels) inChannels = kMaxChannels;
  if (outChannels > kMaxChannels) outChannels = kMaxChannels;
  for (std::uint32_t o = 0; o < outChannels; ++o) {
    bool written = accumulate;
    for (std::uint32_t i = 0; i < inChannels; ++i) {
      const float g0 = from.gain[i][o], g1 = to.gain[i][o];
      if (g0 == 0.0f && g1 == 0.0f) continue;
      if (written) {
        MixGainRamp(out[o], in[i], frames, g0, g1);
      } else {
        CopyGainRamp(out[o], in[i], frames, g0, g1);
        written = true;
      }
    }
    if (!written) Clear(out[o], frames);
  }
}

}  // namespace snd::dsp
