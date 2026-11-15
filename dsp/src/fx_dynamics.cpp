// Dynamics: compressor (with sidechain, used for ducking), look-ahead limiter, normalize.
#include <cmath>
#include <cstring>

#include "fx_common.hpp"

namespace snd::dsp::fx {

namespace {
using detail::At;

inline float LinToDb(float x) noexcept { return 20.0f * std::log10(x > 1e-9f ? x : 1e-9f); }
inline float FastDbToLin(float db) noexcept { return std::exp(db * 0.11512925465f); }  // ln(10)/20

float FrameMax(const float* const* ch, std::uint32_t channels, std::uint32_t i) noexcept {
  float m = 0.0f;
  for (std::uint32_t c = 0; c < channels; ++c) {
    const float a = std::fabs(ch[c][i]);
    m = a > m ? a : m;
  }
  return m;
}
}  // namespace

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------
namespace {
struct CompressorState {
  FxHeader h;
  float envDb;  // smoothed gain change in dB (<= 0)
  float pad[15];
};
}  // namespace

// Static characteristic (gain change in dB, <= 0) for a detector level in dB.
float CompressorGainDb(float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept {
  const float over = levelDb - thresholdDb;
  const float slope = 1.0f / ratio - 1.0f;
  if (kneeDb > 0.0f && 2.0f * std::fabs(over) <= kneeDb) {
    const float x = over + 0.5f * kneeDb;
    return slope * x * x / (2.0f * kneeDb);
  }
  return over > 0.0f ? slope * over : 0.0f;
}

std::size_t CompressorLayout(void*, const FxConfig&) noexcept { return sizeof(CompressorState); }
void CompressorInit(void* state, const FxConfig&) noexcept {
  auto* s = static_cast<CompressorState*>(state);
  s->envDb = 0.0f;
  s->h.tailFrames = 1;
}
void CompressorReset(void* state) noexcept { static_cast<CompressorState*>(state)->envDb = 0.0f; }

void CompressorProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<CompressorState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels;
  const float sr = static_cast<float>(s.h.sampleRate);
  const float inv = 1.0f / static_cast<float>(n);
  const bool useSc = io.sidechain != nullptr && io.sidechain->channelCount > 0 &&
                     io.sidechain->channels != nullptr && p.e[7] >= 0.5f;
  const float att0 = detail::TimeCoef(p.s[2] * 0.001f, sr), att1 = detail::TimeCoef(p.e[2] * 0.001f, sr);
  const float rel0 = detail::TimeCoef(p.s[3] * 0.001f, sr), rel1 = detail::TimeCoef(p.e[3] * 0.001f, sr);
  float env = s.envDb;
  float peak = 0.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    float det;
    if (useSc) {
      det = FrameMax(io.sidechain->channels,
                     io.sidechain->channelCount < kMaxChannels ? io.sidechain->channelCount : kMaxChannels, i) *
            FastDbToLin(p.At(6, t));
    } else {
      det = FrameMax(io.in, chs, i) * FastDbToLin(p.At(6, t));
    }
    const float target = CompressorGainDb(LinToDb(det), p.At(0, t), p.At(1, t), p.At(4, t));
    const float a = target < env ? att0 + (att1 - att0) * t : rel0 + (rel1 - rel0) * t;
    env = target + (env - target) * a;
    const float g = FastDbToLin(env + p.At(5, t));
    for (std::uint32_t c = 0; c < chs; ++c) {
      const float y = io.in[c][i] * g;
      io.out[c][i] = y;
      const float ay = std::fabs(y);
      peak = ay > peak ? ay : peak;
    }
  }
  s.envDb = std::fabs(env) < 1e-6f ? 0.0f : env;
  TailUpdate(s.h, peak, n);
  TailShouldClear(s.h);
}

// ---------------------------------------------------------------------------
// Limiter: gain = box-average over L of (release-smoothed sliding minimum over L
// of the required gain); the signal is delayed by L - 1 frames, so the gain is
// already down when a peak leaves the delay. Final hard clamp at the ceiling.
// ---------------------------------------------------------------------------
namespace {
struct LimiterState {
  FxHeader h;
  std::uint32_t look;      // L (frames, >= 1)
  std::uint32_t delayOff;  // channels x L ring
  std::uint32_t boxOff;    // L floats
  std::uint32_t dqValOff;  // L + 1 floats
  std::uint32_t dqIdxOff;  // L + 1 u32
  std::uint32_t pos;       // ring / box position
  std::uint32_t dqHead, dqTail;  // deque ring indices (capacity L + 1)
  std::uint32_t counter;   // sample counter for the deque
  float rel;               // release-smoothed gain
  double boxSum;
};
}  // namespace

std::size_t LimiterLayout(void* state, const FxConfig& cfg) noexcept {
  detail::Carver c(state, sizeof(LimiterState));
  std::uint32_t look = MsToFrames(cfg.init[2], cfg.sizingRate);
  if (look < 1u) look = 1u;
  const std::uint32_t delay = c.Take<float>(static_cast<std::size_t>(look) * cfg.channels);
  const std::uint32_t box = c.Take<float>(look);
  const std::uint32_t dv = c.Take<float>(look + 1u);
  const std::uint32_t di = c.Take<std::uint32_t>(look + 1u);
  if (state != nullptr) {
    auto* s = static_cast<LimiterState*>(state);
    s->look = look;
    s->delayOff = delay;
    s->boxOff = box;
    s->dqValOff = dv;
    s->dqIdxOff = di;
  }
  return c.Size();
}

void LimiterReset(void* state) noexcept {
  auto* s = static_cast<LimiterState*>(state);
  std::memset(At<float>(state, s->delayOff), 0, sizeof(float) * s->look * s->h.channels);
  float* box = At<float>(state, s->boxOff);
  for (std::uint32_t i = 0; i < s->look; ++i) box[i] = 1.0f;
  s->boxSum = static_cast<double>(s->look);
  s->pos = 0;
  s->dqHead = s->dqTail = 0;
  s->counter = 0;
  s->rel = 1.0f;
}

void LimiterInit(void* state, const FxConfig&) noexcept {
  auto* s = static_cast<LimiterState*>(state);
  LimiterReset(state);
  s->h.latency = s->look - 1u;
  s->h.tailFrames = s->look + 1u;
}

void LimiterProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<LimiterState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels, L = s.look, cap = L + 1u;
  const float sr = static_cast<float>(s.h.sampleRate);
  const float inv = 1.0f / static_cast<float>(n);
  float* ring = At<float>(state, s.delayOff);
  float* box = At<float>(state, s.boxOff);
  float* dqv = At<float>(state, s.dqValOff);
  std::uint32_t* dqi = At<std::uint32_t>(state, s.dqIdxOff);
  const float rc0 = detail::TimeCoef(p.s[1] * 0.001f, sr), rc1 = detail::TimeCoef(p.e[1] * 0.001f, sr);
  // Exact running sum at block start (no drift).
  double sum = 0.0;
  for (std::uint32_t k = 0; k < L; ++k) sum += box[k];
  float peak = 0.0f;
  float xin[kMaxChannels];
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float ceil = DbToGain(p.At(0, t));
    const float gin = DbToGain(p.At(3, t));
    float m = 0.0f;
    for (std::uint32_t c = 0; c < chs; ++c) {
      xin[c] = io.in[c][i] * gin;
      const float a = std::fabs(xin[c]);
      m = a > m ? a : m;
    }
    const float r = m > ceil ? ceil / m : 1.0f;
    // Sliding minimum over the last L required gains.
    const std::uint32_t idx = s.counter++;
    while (s.dqHead != s.dqTail) {
      const std::uint32_t back = s.dqTail == 0 ? cap - 1u : s.dqTail - 1u;
      if (dqv[back] < r) break;
      s.dqTail = back;
    }
    dqv[s.dqTail] = r;
    dqi[s.dqTail] = idx;
    s.dqTail = s.dqTail + 1u == cap ? 0u : s.dqTail + 1u;
    while (idx - dqi[s.dqHead] >= L) s.dqHead = s.dqHead + 1u == cap ? 0u : s.dqHead + 1u;
    const float g1 = dqv[s.dqHead];
    const float rc = rc0 + (rc1 - rc0) * t;
    float rel = s.rel + (1.0f - s.rel) * (1.0f - rc);
    rel = rel < g1 ? rel : g1;
    s.rel = rel;
    sum += static_cast<double>(rel) - static_cast<double>(box[s.pos]);
    box[s.pos] = rel;
    const float g2 = static_cast<float>(sum / static_cast<double>(L));
    for (std::uint32_t c = 0; c < chs; ++c) {
      float* rc_ = ring + static_cast<std::size_t>(c) * L;
      rc_[s.pos] = xin[c];
      const std::uint32_t rd = s.pos + 1u == L ? 0u : s.pos + 1u;
      float y = rc_[rd] * g2;
      y = detail::Clamp(y, -ceil, ceil);
      io.out[c][i] = y;
      const float ay = std::fabs(y);
      peak = ay > peak ? ay : peak;
    }
    s.pos = s.pos + 1u == L ? 0u : s.pos + 1u;
  }
  s.boxSum = sum;
  TailUpdate(s.h, peak, n);
  TailShouldClear(s.h);
}

// ---------------------------------------------------------------------------
// Normalize (Unity): gain = min(MaximumAmp, 1 / max(peak, LowestVolume)),
// falling immediately, rising with the FadeIn time constant.
// ---------------------------------------------------------------------------
namespace {
struct NormalizeState {
  FxHeader h;
  float peak;
  float gain;
  float pad[14];
};
constexpr float kNormalizeReleaseSeconds = 3.0f;
}  // namespace

std::size_t NormalizeLayout(void*, const FxConfig&) noexcept { return sizeof(NormalizeState); }
void NormalizeReset(void* state) noexcept {
  auto* s = static_cast<NormalizeState*>(state);
  s->peak = 0.0f;
  s->gain = 1.0f;
}
void NormalizeInit(void* state, const FxConfig&) noexcept {
  NormalizeReset(state);
  static_cast<NormalizeState*>(state)->h.tailFrames = 1;
}

void NormalizeProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<NormalizeState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels;
  const float sr = static_cast<float>(s.h.sampleRate);
  const float inv = 1.0f / static_cast<float>(n);
  const float decay = detail::TimeCoef(kNormalizeReleaseSeconds, sr);
  const float up0 = 1.0f - detail::TimeCoef(p.s[0] * 0.001f, sr);
  const float up1 = 1.0f - detail::TimeCoef(p.e[0] * 0.001f, sr);
  float peakOut = 0.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float m = FrameMax(io.in, chs, i);
    s.peak = m > s.peak * decay ? m : s.peak * decay;
    const float floorV = p.At(1, t);
    const float level = s.peak > floorV ? s.peak : floorV;
    float desired = level > 0.0f ? 1.0f / level : p.At(2, t);
    desired = desired < p.At(2, t) ? desired : p.At(2, t);
    if (desired < s.gain) {
      s.gain = desired;
    } else {
      s.gain += (desired - s.gain) * (up0 + (up1 - up0) * t);
    }
    for (std::uint32_t c = 0; c < chs; ++c) {
      const float y = io.in[c][i] * s.gain;
      io.out[c][i] = y;
      const float a = std::fabs(y);
      peakOut = a > peakOut ? a : peakOut;
    }
  }
  if (s.peak < detail::kFlushThreshold) s.peak = 0.0f;
  TailUpdate(s.h, peakOut, n);
  TailShouldClear(s.h);
}

}  // namespace snd::dsp::fx
