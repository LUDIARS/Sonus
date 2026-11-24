// Delay-line effects: echo, delay, chorus, flanger, pitch shifter.
#include <cmath>
#include <cstring>

#include "fx_common.hpp"

namespace snd::dsp::fx {

namespace {

using detail::At;
using detail::ReadFrac;

// Common shape: header + per-channel rings of `size` frames.
struct RingState {
  FxHeader h;
  std::uint32_t ringOff;
  std::uint32_t size;   // frames per channel ring
  std::uint32_t write;  // next write index (shared by channels)
  float maxDelay;       // echo / delay: MaxDelay in frames at the run rate (0: not used)
  float lp[kMaxChannels];      // damping filter state (delay)
  double phase;                // LFO / pitch window phase [0, 1)
  float fb[kMaxChannels];      // last wet output per channel (feedback)
};

std::size_t RingLayout(void* state, const FxConfig& cfg, float capacityMs) noexcept {
  detail::Carver c(state, sizeof(RingState));
  const std::uint32_t size = MsToFrames(capacityMs, cfg.sizingRate) + 4u;
  const std::uint32_t off = c.Take<float>(static_cast<std::size_t>(size) * cfg.channels);
  if (state != nullptr) {
    auto* s = static_cast<RingState*>(state);
    s->ringOff = off;
    s->size = size;
    s->write = 0;
  }
  return c.Size();
}

void RingReset(void* state) noexcept {
  auto* s = static_cast<RingState*>(state);
  std::memset(At<float>(state, s->ringOff), 0,
              static_cast<std::size_t>(s->size) * s->h.channels * sizeof(float));
  std::memset(s->lp, 0, sizeof(s->lp));
  std::memset(s->fb, 0, sizeof(s->fb));
  s->write = 0;
}

float* Ring(void* state, std::uint32_t c) noexcept {
  auto* s = static_cast<RingState*>(state);
  return At<float>(state, s->ringOff) + static_cast<std::size_t>(c) * s->size;
}

// Delay in frames from milliseconds at the run rate. Stage 3 (re-check 2): a delay above
// MaxDelay is exactly MaxDelay (the same frames as Delay = MaxDelay), at any rate; the
// ring (sized at the sizing rate, + 4 frames) is the second bound only when the run rate
// is higher than the sizing rate.
float MsFrames(float ms, std::uint32_t rate) noexcept { return ms * static_cast<float>(rate) / 1000.0f; }
float DelayFrames(const RingState& s, float ms) noexcept {
  const float d = MsFrames(ms, s.h.sampleRate);
  float hi = static_cast<float>(s.size - 3u);
  if (s.maxDelay > 0.0f && s.maxDelay < hi) hi = s.maxDelay;
  return detail::Clamp(d, 1.0f, hi);
}

void FinishBlock(RingState& s, std::uint32_t frames, float peak) noexcept {
  s.write = (s.write + frames) % s.size;
  TailUpdate(s.h, peak, frames);
  if (TailShouldClear(s.h)) {
    RingReset(&s);
  } else {
    for (std::uint32_t c = 0; c < s.h.channels; ++c) {
      s.lp[c] = detail::Flush(s.lp[c]);
      s.fb[c] = detail::Flush(s.fb[c]);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Echo: out = dry * x + wet * d, line <- x + decay * d
// ---------------------------------------------------------------------------
std::size_t EchoLayout(void* state, const FxConfig& cfg) noexcept {
  return RingLayout(state, cfg, cfg.init[4]);
}
void EchoInit(void* state, const FxConfig& cfg) noexcept {
  auto* s = static_cast<RingState*>(state);
  s->h.tailFrames = s->size;
  s->maxDelay = MsFrames(cfg.init[4], cfg.sampleRate);
}
void EchoReset(void* state) noexcept { RingReset(state); }

void EchoProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<RingState*>(state);
  const std::uint32_t n = io.frames;
  const float inv = 1.0f / static_cast<float>(n);
  const float d0 = DelayFrames(s, p.s[0]), d1 = DelayFrames(s, p.e[0]);
  float peak = 0.0f;
  for (std::uint32_t c = 0; c < io.channels; ++c) {
    float* ring = Ring(state, c);
    const float* x = io.in[c];
    float* y = io.out[c];
    std::uint32_t w = s.write;
    for (std::uint32_t i = 0; i < n; ++i) {
      const float t = static_cast<float>(i) * inv;
      const float d = ReadFrac(ring, s.size, w, (d0 + (d1 - d0) * t) - 1.0f);
      const float xi = x[i];
      const float v = detail::FlushState(xi + p.At(1, t) * d);
      ring[w] = v;
      w = w + 1 == s.size ? 0 : w + 1;
      y[i] = p.At(2, t) * xi + p.At(3, t) * d;
      const float a = std::fabs(v);
      peak = a > peak ? a : peak;
    }
  }
  FinishBlock(s, n, peak);
}

// ---------------------------------------------------------------------------
// Delay: feedback through a one-pole damping lowpass, optional ping-pong.
// ---------------------------------------------------------------------------
std::size_t DelayLayout(void* state, const FxConfig& cfg) noexcept {
  return RingLayout(state, cfg, cfg.init[6]);
}
void DelayInit(void* state, const FxConfig& cfg) noexcept {
  auto* s = static_cast<RingState*>(state);
  s->h.tailFrames = s->size;
  s->maxDelay = MsFrames(cfg.init[6], cfg.sampleRate);
}
void DelayReset(void* state) noexcept { RingReset(state); }

void DelayProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<RingState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels;
  const float inv = 1.0f / static_cast<float>(n);
  const float d0 = DelayFrames(s, p.s[0]), d1 = DelayFrames(s, p.e[0]);
  const float dry0 = DbToGain(p.s[2]), dry1 = DbToGain(p.e[2]);
  const float wet0 = DbToGain(p.s[3]), wet1 = DbToGain(p.e[3]);
  const float sr = static_cast<float>(s.h.sampleRate);
  // One-pole damping coefficients at block start/end (20 kHz = open).
  auto damp = [sr](float hz) {
    return hz >= 19999.0f ? 0.0f : std::exp(-2.0f * detail::kPiF * hz / sr);
  };
  const float a0 = damp(p.s[4]), a1 = damp(p.e[4]);
  float* rings[kMaxChannels];
  for (std::uint32_t c = 0; c < chs; ++c) rings[c] = Ring(state, c);
  float peak = 0.0f;
  std::uint32_t w = s.write;
  float d[kMaxChannels];
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float delay = (d0 + (d1 - d0) * t) - 1.0f;
    const float fbk = p.At(1, t), pp = p.At(5, t);
    const float a = a0 + (a1 - a0) * t;
    const float dry = dry0 + (dry1 - dry0) * t, wet = wet0 + (wet1 - wet0) * t;
    for (std::uint32_t c = 0; c < chs; ++c) d[c] = ReadFrac(rings[c], s.size, w, delay);
    for (std::uint32_t c = 0; c < chs; ++c) {
      // Ping-pong: the feedback of a stereo pair crosses over.
      const float other = (chs >= 2 && c < 2) ? d[c ^ 1u] : d[c];
      const float f = d[c] + (other - d[c]) * pp;
      s.lp[c] = detail::FlushState(f + (s.lp[c] - f) * a);
      const float xi = io.in[c][i];
      const float v = detail::FlushState(xi + fbk * s.lp[c]);
      rings[c][w] = v;
      io.out[c][i] = dry * xi + wet * d[c];
      const float av = std::fabs(v);
      peak = av > peak ? av : peak;
    }
    w = w + 1 == s.size ? 0 : w + 1;
  }
  FinishBlock(s, n, peak);
}

// ---------------------------------------------------------------------------
// Chorus / flanger: modulated taps.
// ---------------------------------------------------------------------------
namespace {

struct ModSpec {
  std::uint32_t taps;
  float depthScale;  // modulation amplitude = depth * base delay * depthScale
};

void ModProcess(RingState& s, void* state, const FxIo& io, const FxParams& p,
                const ModSpec& spec) noexcept {
  const std::uint32_t n = io.frames, chs = io.channels;
  const float inv = 1.0f / static_cast<float>(n);
  const float sr = static_cast<float>(s.h.sampleRate);
  const float maxD = static_cast<float>(s.size - 3u);
  float* rings[kMaxChannels];
  for (std::uint32_t c = 0; c < chs; ++c) rings[c] = Ring(state, c);
  float peak = 0.0f;
  std::uint32_t w = s.write;
  double phase = s.phase;
  const float tapInv = 1.0f / static_cast<float>(spec.taps);
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float base = p.At(2, t) * 0.001f * sr;
    const float amp = p.At(4, t) * base * spec.depthScale;
    const float dryM = p.At(0, t), wetM = p.At(1, t), fbk = p.At(5, t);
    for (std::uint32_t c = 0; c < chs; ++c) {
      float wet = 0.0f;
      for (std::uint32_t k = 0; k < spec.taps; ++k) {
        const double ph = phase + static_cast<double>(k) * tapInv + 0.25 * static_cast<double>(c);
        const float lfo = static_cast<float>(std::sin(2.0 * detail::kPi * ph));
        const float dl = detail::Clamp(base + amp * lfo, 1.0f, maxD);
        wet += ReadFrac(rings[c], s.size, w, dl - 1.0f);
      }
      wet *= tapInv;
      const float xi = io.in[c][i];
      const float v = detail::FlushState(xi + fbk * wet);
      rings[c][w] = v;
      io.out[c][i] = dryM * xi + wetM * wet;
      const float av = std::fabs(v);
      peak = av > peak ? av : peak;
    }
    w = w + 1 == s.size ? 0 : w + 1;
    phase += static_cast<double>(p.At(3, t)) / static_cast<double>(sr);
    if (phase >= 1.0) phase -= std::floor(phase);
  }
  s.phase = phase;
  FinishBlock(s, n, peak);
}

}  // namespace

std::size_t ChorusLayout(void* state, const FxConfig& cfg) noexcept {
  return RingLayout(state, cfg, 80.0f);  // 50 ms base + 50 % modulation + margin
}
void ChorusInit(void* state, const FxConfig&) noexcept {
  auto* s = static_cast<RingState*>(state);
  s->h.tailFrames = s->size;
  s->phase = 0.0;
}
void ChorusReset(void* state) noexcept { RingReset(state); }
void ChorusProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  ModProcess(*static_cast<RingState*>(state), state, io, p, ModSpec{3u, 0.5f});
}

std::size_t FlangerLayout(void* state, const FxConfig& cfg) noexcept {
  return RingLayout(state, cfg, 25.0f);  // 10 ms base swept to 2x + margin
}
void FlangerInit(void* state, const FxConfig&) noexcept {
  auto* s = static_cast<RingState*>(state);
  s->h.tailFrames = s->size;
  s->phase = 0.0;
}
void FlangerReset(void* state) noexcept { RingReset(state); }
void FlangerProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  ModProcess(*static_cast<RingState*>(state), state, io, p, ModSpec{1u, 1.0f});
}

// ---------------------------------------------------------------------------
// Pitch shifter: two taps sweeping through a window, sin^2 crossfade.
// At Pitch = 1 the taps rest at delay 0 (weight 0) and W/2 (weight 1), so the
// effect delays by W/2 frames (W = WindowMs); the dry part of Mix is delayed by
// the same W/2, so Mix never makes a comb filter and Pitch = 1 is the input
// delayed by EffectLatencyFrames() = W/2, bit-exactly, at any sample rate:
// W is WindowMs in frames rounded to an EVEN number of frames, so W/2 is an
// integer delay (no interpolation). While WindowMs ramps, W moves linearly
// between the even windows of the block's start and end values.
// ---------------------------------------------------------------------------
namespace {
float PitchWindowFrames(const RingState& s, float ms) noexcept {
  const float maxEven = static_cast<float>((s.size - 4u) & ~1u);
  const float half = std::nearbyint(ms * static_cast<float>(s.h.sampleRate) / 2000.0f);
  return detail::Clamp(2.0f * half, 8.0f, maxEven);
}
}  // namespace
std::size_t PitchShifterLayout(void* state, const FxConfig& cfg) noexcept {
  return RingLayout(state, cfg, 200.0f);
}
void PitchShifterInit(void* state, const FxConfig& cfg) noexcept {
  auto* s = static_cast<RingState*>(state);
  s->h.tailFrames = s->size;
  s->phase = 0.0;
  s->h.latency = static_cast<std::uint32_t>(0.5f * PitchWindowFrames(*s, cfg.init[1]));
}
void PitchShifterReset(void* state) noexcept {
  RingReset(state);
  static_cast<RingState*>(state)->phase = 0.0;
}

void PitchShifterProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  auto& s = *static_cast<RingState*>(state);
  const std::uint32_t n = io.frames, chs = io.channels;
  const float inv = 1.0f / static_cast<float>(n);
  const float sr = static_cast<float>(s.h.sampleRate);
  float* rings[kMaxChannels];
  for (std::uint32_t c = 0; c < chs; ++c) rings[c] = Ring(state, c);
  float peak = 0.0f;
  std::uint32_t w = s.write;
  double phase = s.phase;
  (void)sr;
  const float win0 = PitchWindowFrames(s, p.s[1]), win1 = PitchWindowFrames(s, p.e[1]);
  for (std::uint32_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float win = win0 == win1 ? win0 : win0 + (win1 - win0) * t;
    const float mix = p.At(2, t);
    const float ph1 = static_cast<float>(phase);
    float ph2 = ph1 + 0.5f;
    if (ph2 >= 1.0f) ph2 -= 1.0f;
    const float g1 = std::sin(detail::kPiF * ph1);
    const float w1 = g1 * g1, w2 = 1.0f - w1;  // sin^2 + cos^2 = 1 (w2 = 1 exactly at rest)
    (void)ph2;
    // Write first, then read with delay 0 = the current frame.
    const std::uint32_t wn = w + 1 == s.size ? 0 : w + 1;
    for (std::uint32_t c = 0; c < chs; ++c) {
      const float xi = io.in[c][i];
      rings[c][w] = xi;
      const float a = ReadFrac(rings[c], s.size, wn, ph1 * win);
      const float b = ReadFrac(rings[c], s.size, wn, ph2 * win);
      const float dry = ReadFrac(rings[c], s.size, wn, 0.5f * win);
      const float wet = w1 * a + w2 * b;
      io.out[c][i] = dry + mix * (wet - dry);  // == dry exactly when wet == dry
      const float av = std::fabs(xi);
      peak = av > peak ? av : peak;
    }
    w = w + 1 == s.size ? 0 : w + 1;
    phase += static_cast<double>((1.0f - p.At(0, t)) / win);
    phase -= std::floor(phase);
  }
  s.phase = phase;
  s.h.latency = static_cast<std::uint32_t>(0.5f * win1);
  FinishBlock(s, n, peak);
}

}  // namespace snd::dsp::fx
