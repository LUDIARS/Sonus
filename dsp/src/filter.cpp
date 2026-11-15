#include "snd/dsp/filter.hpp"

#include <cmath>
#include <cstring>

#include "detail.hpp"

namespace snd::dsp {

namespace {

// ---------------------------------------------------------------------------
// Voice filter (TPT SVF). FilterState.data layout:
//   [0..8)   lowpass ic1 per channel     [8..16)  lowpass ic2
//   [16..24) highpass ic1                [24..32) highpass ic2
//   [32] lowpass Hz used last  [33] highpass Hz used last
//   [34] lowpass active (0/1)  [35] highpass active  [36] initialised
// ---------------------------------------------------------------------------
constexpr std::uint32_t kLpIc1 = 0, kLpIc2 = 8, kHpIc1 = 16, kHpIc2 = 24;
constexpr std::uint32_t kLpHz = 32, kHpHz = 33, kLpOn = 34, kHpOn = 35, kInit = 36;
constexpr float kSqrt2 = 1.41421356237f;  // k = 1/Q, Butterworth
constexpr std::uint32_t kSub = 16;

struct Svf {
  float a1, a2, a3;
};

float SvfG(float hz, float sampleRate) noexcept { return std::tan(detail::kPiF * hz / sampleRate); }

Svf SvfFromG(float g) noexcept {
  Svf s;
  s.a1 = 1.0f / (1.0f + g * (g + kSqrt2));
  s.a2 = g * s.a1;
  s.a3 = g * s.a2;
  return s;
}

float LpOpenLimit(float sr) noexcept { return 0.45f * sr; }

bool LpActive(float hz, float sr) noexcept {
  return hz < kVoiceLowpassOffHz && hz < LpOpenLimit(sr);
}
bool HpActive(float hz) noexcept { return hz > kVoiceHighpassOffHz; }

float ClampHz(float hz, float sr) noexcept {
  if (!(hz == hz)) hz = 1000.0f;  // NaN
  return detail::Clamp(hz, 10.0f, LpOpenLimit(sr));
}

// Runs one SVF over the block. The prewarped gain g = tan(pi f / fs) moves
// linearly from g(f0) to g(f1) (two tan() per block, one division per 16 frames).
template <bool kLowpass>
void RunSvf(float* ic1, float* ic2, float* const* buf, std::uint32_t channels,
            std::uint32_t frames, float f0, float f1, float sr) noexcept {
  const bool sweep = f0 != f1;
  const float g0 = SvfG(f0, sr);
  const float g1 = sweep ? SvfG(f1, sr) : g0;
  Svf cf = SvfFromG(g0);
  for (std::uint32_t s = 0; s < frames; s += kSub) {
    const std::uint32_t n = frames - s < kSub ? frames - s : kSub;
    if (sweep) {
      const float t = static_cast<float>(s) / static_cast<float>(frames);
      cf = SvfFromG(g0 + (g1 - g0) * t);
    }
    for (std::uint32_t c = 0; c < channels; ++c) {
      float* x = buf[c] + s;
      float i1 = ic1[c], i2 = ic2[c];
      for (std::uint32_t k = 0; k < n; ++k) {
        const float v0 = x[k];
        const float v3 = v0 - i2;
        const float v1 = cf.a1 * i1 + cf.a2 * v3;
        const float v2 = i2 + cf.a2 * i1 + cf.a3 * v3;
        i1 = detail::FlushState(2.0f * v1 - i1);
        i2 = detail::FlushState(2.0f * v2 - i2);
        x[k] = kLowpass ? v2 : (v0 - kSqrt2 * v1 - v2);
      }
      ic1[c] = i1;
      ic2[c] = i2;
    }
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    ic1[c] = detail::Flush(ic1[c]);
    ic2[c] = detail::Flush(ic2[c]);
  }
}

}  // namespace

void VoiceFilterInit(FilterState& state, const FilterTarget& initial) noexcept {
  std::memset(&state, 0, sizeof(state));
  state.data[kLpHz] = initial.lowpassHz;
  state.data[kHpHz] = initial.highpassHz;
  state.data[kInit] = 1.0f;
}

bool VoiceFilterIsOpen(const FilterState& state, std::uint32_t sampleRate) noexcept {
  const float sr = static_cast<float>(sampleRate);
  return !LpActive(state.data[kLpHz], sr) && !HpActive(state.data[kHpHz]);
}

void VoiceFilterProcess(FilterState& state, float* const* buffers, std::uint32_t channels,
                        std::uint32_t frames, const FilterTarget& target,
                        std::uint32_t sampleRate) noexcept {
  FilterTarget start = target;
  if (state.data[kInit] != 0.0f) {
    start.lowpassHz = state.data[kLpHz];
    start.highpassHz = state.data[kHpHz];
  }
  VoiceFilterProcessRamp(state, buffers, channels, frames, start, target, sampleRate);
}

void VoiceFilterProcessRamp(FilterState& state, float* const* buffers, std::uint32_t channels,
                            std::uint32_t frames, const FilterTarget& start,
                            const FilterTarget& end, std::uint32_t sampleRate) noexcept {
  if (channels > kMaxChannels) channels = kMaxChannels;
  const float sr = static_cast<float>(sampleRate);
  float* d = state.data;
  d[kInit] = 1.0f;
  d[kLpHz] = end.lowpassHz;
  d[kHpHz] = end.highpassHz;
  if (frames == 0) return;

  // Lowpass.
  {
    const bool on0 = LpActive(start.lowpassHz, sr), on1 = LpActive(end.lowpassHz, sr);
    if (on0 || on1) {
      if (d[kLpOn] == 0.0f) {
        // Switching on: steady state for the current input (lowpass output = input).
        for (std::uint32_t c = 0; c < channels; ++c) {
          d[kLpIc1 + c] = 0.0f;
          d[kLpIc2 + c] = buffers[c][0];
        }
      }
      const float f0 = on0 ? ClampHz(start.lowpassHz, sr) : LpOpenLimit(sr);
      const float f1 = on1 ? ClampHz(end.lowpassHz, sr) : LpOpenLimit(sr);
      RunSvf<true>(d + kLpIc1, d + kLpIc2, buffers, channels, frames, f0, f1, sr);
      d[kLpOn] = on1 ? 1.0f : 0.0f;
    } else {
      d[kLpOn] = 0.0f;
    }
  }
  // Highpass.
  {
    const bool on0 = HpActive(start.highpassHz), on1 = HpActive(end.highpassHz);
    if (on0 || on1) {
      if (d[kHpOn] == 0.0f) {
        // Switching on: internal lowpass at 0 (the mean of audio), output = input.
        for (std::uint32_t c = 0; c < channels; ++c) {
          d[kHpIc1 + c] = 0.0f;
          d[kHpIc2 + c] = 0.0f;
        }
      }
      const float f0 = on0 ? ClampHz(start.highpassHz, sr) : 10.0f;
      const float f1 = on1 ? ClampHz(end.highpassHz, sr) : 10.0f;
      RunSvf<false>(d + kHpIc1, d + kHpIc2, buffers, channels, frames, f0, f1, sr);
      d[kHpOn] = on1 ? 1.0f : 0.0f;
    } else {
      d[kHpOn] = 0.0f;
    }
  }
}

// ---------------------------------------------------------------------------
// Biquads
// ---------------------------------------------------------------------------
BiquadCoefs DesignBiquad(BiquadType type, float freqHz, float q, float gainDb,
                         float sampleRate) noexcept {
  if (!(sampleRate > 0.0f)) return kBiquadIdentity;
  const double fs = sampleRate;
  double f = freqHz == freqHz ? freqHz : 1000.0;
  f = detail::Clamp(f, 1.0, 0.49 * fs);
  const bool shelf = type == BiquadType::LowShelf || type == BiquadType::HighShelf;
  double qq = q == q ? q : 0.7071;
  if (!shelf || qq > 0.0) qq = detail::Clamp(qq, 0.05, 100.0);
  const double gdb = gainDb == gainDb ? detail::Clamp(static_cast<double>(gainDb), -60.0, 60.0) : 0.0;
  const double w0 = 2.0 * detail::kPi * f / fs;
  const double cw = std::cos(w0), sw = std::sin(w0);
  const double A = std::pow(10.0, gdb / 40.0);
  double alpha = sw / (2.0 * qq);
  if (shelf && !(qq > 0.0)) {
    // Shelf slope S = 1: alpha = sin(w0)/2 * sqrt((A + 1/A)(1/S - 1) + 2).
    alpha = sw / 2.0 * std::sqrt(2.0);
  }
  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  switch (type) {
    case BiquadType::Lowpass:
      b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BiquadType::Highpass:
      b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BiquadType::Bandpass:
      b0 = alpha; b1 = 0; b2 = -alpha;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BiquadType::Notch:
      b0 = 1; b1 = -2 * cw; b2 = 1;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BiquadType::Peaking:
      b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
      a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
      break;
    case BiquadType::LowShelf: {
      const double sa = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cw + sa);
      b1 = 2 * A * ((A - 1) - (A + 1) * cw);
      b2 = A * ((A + 1) - (A - 1) * cw - sa);
      a0 = (A + 1) + (A - 1) * cw + sa;
      a1 = -2 * ((A - 1) + (A + 1) * cw);
      a2 = (A + 1) + (A - 1) * cw - sa;
      break;
    }
    case BiquadType::HighShelf: {
      const double sa = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cw + sa);
      b1 = -2 * A * ((A - 1) + (A + 1) * cw);
      b2 = A * ((A + 1) + (A - 1) * cw - sa);
      a0 = (A + 1) - (A - 1) * cw + sa;
      a1 = 2 * ((A - 1) - (A + 1) * cw);
      a2 = (A + 1) - (A - 1) * cw - sa;
      break;
    }
    case BiquadType::Allpass:
      b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
  }
  BiquadCoefs c;
  c.b0 = static_cast<float>(b0 / a0);
  c.b1 = static_cast<float>(b1 / a0);
  c.b2 = static_cast<float>(b2 / a0);
  c.a1 = static_cast<float>(a1 / a0);
  c.a2 = static_cast<float>(a2 / a0);
  return c;
}

double BiquadMagnitude(const BiquadCoefs& c, double freqHz, double sampleRate) noexcept {
  const double w = 2.0 * detail::kPi * freqHz / sampleRate;
  const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
  const double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
  const double dr = 1.0 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
  return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

float OctaveBandwidthToQ(float octaves) noexcept {
  const double n = detail::Clamp(static_cast<double>(octaves), 0.01, 10.0);
  const double p = std::pow(2.0, n);
  return static_cast<float>(std::sqrt(p) / (p - 1.0));
}

void BiquadReset(BiquadState& s) noexcept { std::memset(&s, 0, sizeof(s)); }

void BiquadFlush(BiquadState& s, std::uint32_t channels) noexcept {
  for (std::uint32_t c = 0; c < channels && c < kMaxChannels; ++c) {
    s.z1[c] = detail::Flush(s.z1[c]);
    s.z2[c] = detail::Flush(s.z2[c]);
  }
}

void BiquadProcess(BiquadState& s, const BiquadCoefs& from, const BiquadCoefs& to,
                   const float* const* in, float* const* out, std::uint32_t channels,
                   std::uint32_t frames) noexcept {
  if (channels > kMaxChannels) channels = kMaxChannels;
  const bool ramp = std::memcmp(&from, &to, sizeof(BiquadCoefs)) != 0;
  for (std::uint32_t c = 0; c < channels; ++c) {
    const float* x = in[c];
    float* y = out[c];
    float z1 = s.z1[c], z2 = s.z2[c];
    if (!ramp) {
      const float b0 = from.b0, b1 = from.b1, b2 = from.b2, a1 = from.a1, a2 = from.a2;
      for (std::uint32_t i = 0; i < frames; ++i) {
        const float xi = x[i];
        const float yi = b0 * xi + z1;
        z1 = detail::FlushState(b1 * xi - a1 * yi + z2);
        z2 = detail::FlushState(b2 * xi - a2 * yi);
        y[i] = yi;
      }
    } else {
      const float inv = 1.0f / static_cast<float>(frames);
      const float db0 = (to.b0 - from.b0) * inv, db1 = (to.b1 - from.b1) * inv,
                  db2 = (to.b2 - from.b2) * inv, da1 = (to.a1 - from.a1) * inv,
                  da2 = (to.a2 - from.a2) * inv;
      for (std::uint32_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i);
        const float b0 = from.b0 + db0 * t, b1 = from.b1 + db1 * t, b2 = from.b2 + db2 * t;
        const float a1 = from.a1 + da1 * t, a2 = from.a2 + da2 * t;
        const float xi = x[i];
        const float yi = b0 * xi + z1;
        z1 = detail::FlushState(b1 * xi - a1 * yi + z2);
        z2 = detail::FlushState(b2 * xi - a2 * yi);
        y[i] = yi;
      }
    }
    s.z1[c] = z1;
    s.z2[c] = z2;
  }
}

}  // namespace snd::dsp
