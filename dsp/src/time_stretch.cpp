// WSOLA time stretch (time_stretch.hpp).
#include "snd/dsp/time_stretch.hpp"

#include <cmath>
#include <cstring>

#include "detail.hpp"
#include "snd/dsp/kernels.hpp"

namespace snd::dsp {

namespace {

constexpr std::uint32_t kTsMagic = 0x31535453u;  // 'STS1'

struct TsState {
  std::uint32_t magic;
  std::uint32_t channels;
  std::uint32_t rate;
  std::uint32_t hop;
  std::uint32_t search;
  std::uint32_t dec;
  std::uint32_t look;
  std::uint32_t pad;
  float maxSpeed;
  float windowMs;
  float searchMs;
  std::uint32_t winOff;   // wr[hop]
  std::uint32_t histOff;  // channels * pad floats: input frames [end - pad, end)
  std::uint32_t monoOff;  // 2 hop + 2 search floats
  std::uint32_t bytes;
  std::uint32_t started;  // hop 0 decided
  std::uint32_t h;        // frames of the current hop already output (hop = finished)
  std::uint32_t reserved;
  std::uint64_t tau;      // nominal position of the current hop, 32.32 fixed point
  std::int64_t pPrev;     // segment start of the previous hop
  std::int64_t pCur;      // segment start of the current hop
  std::int64_t end;       // input frames consumed (absolute index of the next new frame)
  std::uint32_t pad2[2];
};
static_assert(sizeof(TsState) % 8 == 0);

float ClampMs(float v, float lo, float hi, float def) noexcept {
  if (!(v == v)) return def;
  return v < lo ? lo : (v > hi ? hi : v);
}

bool Shape(std::uint32_t rate, float maxSpeed, float windowMs, float searchMs,
           TimeStretchShape* out) noexcept {
  if (rate == 0u || rate > kTimeStretchMaxRate) return false;
  windowMs = ClampMs(windowMs, 5.0f, 200.0f, kTimeStretchDefaultWindowMs);
  searchMs = ClampMs(searchMs, 0.0f, 50.0f, kTimeStretchDefaultSearchMs);
  maxSpeed = ClampMs(maxSpeed, 1.0f, kTimeStretchMaxSpeed, kTimeStretchMaxSpeed);
  std::uint32_t hop = static_cast<std::uint32_t>(
      std::lround(static_cast<double>(rate) * static_cast<double>(windowMs) / 2000.0));
  if (hop < 16u) hop = 16u;
  const std::uint32_t search = static_cast<std::uint32_t>(
      std::lround(static_cast<double>(rate) * static_cast<double>(searchMs) / 1000.0));
  std::uint32_t dec = 1u;
  while (dec * 2u <= hop / 64u) dec *= 2u;
  const std::uint32_t maxAdvance =
      static_cast<std::uint32_t>(std::ceil(static_cast<double>(maxSpeed) * hop));
  out->hopFrames = hop;
  out->searchFrames = search;
  out->decimation = dec;
  out->lookaheadFrames = search + 2u * hop;
  out->padFrames = static_cast<std::uint32_t>(
      detail::AlignUp(std::size_t{2u * search + hop + maxAdvance + 2u}, 16));
  return true;
}

struct Lay {
  TimeStretchShape s;
  std::uint32_t winOff, histOff, monoOff;
  std::size_t bytes;
};

bool MakeLayout(std::uint32_t channels, std::uint32_t rate, float maxSpeed, float windowMs,
                float searchMs, Lay* l) noexcept {
  if (channels == 0u || channels > kMaxChannels) return false;
  if (!Shape(rate, maxSpeed, windowMs, searchMs, &l->s)) return false;
  detail::Carver c(nullptr, sizeof(TsState));
  l->winOff = c.Take<float>(l->s.hopFrames);
  l->histOff = c.Take<float>(std::size_t{channels} * l->s.padFrames);
  l->monoOff = c.Take<float>(2u * l->s.hopFrames + 2u * l->s.searchFrames + 16u);
  l->bytes = c.Size();
  return true;
}

TsState& St(void* s) noexcept { return *static_cast<TsState*>(s); }
const TsState& St(const void* s) noexcept { return *static_cast<const TsState*>(s); }

void ResetDynamic(TsState& s) noexcept {
  s.started = 0;
  s.h = s.hop;
  s.tau = 0;
  s.pPrev = -static_cast<std::int64_t>(s.hop);
  s.pCur = 0;
  s.end = 0;
}

float SpeedAt(const TsState& s, float s0, float s1, std::uint32_t i, std::uint32_t n) noexcept {
  float v = s0 == s1 ? s0 : s0 + (s1 - s0) * (static_cast<float>(i) / static_cast<float>(n));
  if (!(v == v)) v = 1.0f;
  if (v < kTimeStretchMinSpeed) v = kTimeStretchMinSpeed;
  if (v > s.maxSpeed) v = s.maxSpeed;
  return v;
}

std::uint64_t Increment(const TsState& s, float speed) noexcept {
  return static_cast<std::uint64_t>(
      std::llround(static_cast<double>(speed) * static_cast<double>(s.hop) * 4294967296.0));
}

std::int64_t Nominal(std::uint64_t tau) noexcept { return static_cast<std::int64_t>(tau >> 32); }

// Input end (absolute, exclusive) the current call needs; also the number of
// hops decided. Pure simulation of the hop decisions.
std::int64_t Requirement(const TsState& s, std::uint32_t outFrames, float s0, float s1) noexcept {
  std::int64_t req = s.end;
  std::uint32_t h = s.h;
  std::uint32_t started = s.started;
  std::uint64_t tau = s.tau;
  for (std::uint32_t o = 0; o < outFrames;) {
    if (started == 0u || h >= s.hop) {
      if (started == 0u) {
        tau = 0;
        started = 1u;
      } else {
        tau += Increment(s, SpeedAt(s, s0, s1, o, outFrames));
      }
      const std::int64_t r = Nominal(tau) + s.search + 2 * static_cast<std::int64_t>(s.hop);
      if (r > req) req = r;
      h = 0;
    }
    const std::uint32_t len = s.hop - h < outFrames - o ? s.hop - h : outFrames - o;
    h += len;
    o += len;
  }
  return req;
}

// Reads frame `abs` of channel c from the call's buffer (base = absolute index of in[c][0]).
inline float X(float* const* in, std::uint32_t c, std::int64_t base, std::int64_t abs) noexcept {
  return in[c][abs - base];
}

// Best segment start in [lo, hi] for the target x[natural + j], j < hop.
std::int64_t Search(TsState& s, float* const* in, std::int64_t base, std::int64_t natural,
                    std::int64_t lo, std::int64_t hi) noexcept {
  const std::uint32_t hop = s.hop;
  const std::uint32_t span = static_cast<std::uint32_t>(hi - lo) + hop;
  float* tgt = detail::At<float>(&s, s.monoOff);
  float* cand = tgt + hop;
  for (std::uint32_t j = 0; j < hop; ++j) {
    float v = 0.0f;
    for (std::uint32_t c = 0; c < s.channels; ++c) v += X(in, c, base, natural + j);
    tgt[j] = v;
  }
  for (std::uint32_t q = 0; q < span; ++q) {
    float v = 0.0f;
    for (std::uint32_t c = 0; c < s.channels; ++c) v += X(in, c, base, lo + q);
    cand[q] = v;
  }
  // Score = corr * |corr| / energy (orders like the normalised correlation).
  auto score = [&](std::uint32_t off, std::uint32_t step) noexcept {
    double corr = 0.0, en = 0.0;
    for (std::uint32_t j = 0; j < hop; j += step) {
      const double c = cand[off + j];
      corr += static_cast<double>(tgt[j]) * c;
      en += c * c;
    }
    return en > 1e-30 ? corr * std::fabs(corr) / en : 0.0;
  };
  const std::uint32_t range = static_cast<std::uint32_t>(hi - lo);
  const std::uint32_t d = s.dec;
  std::uint32_t best = 0;
  double bestScore = -1e300;
  for (std::uint32_t off = 0; off <= range; off += d) {
    const double v = score(off, d);
    if (v > bestScore) {
      bestScore = v;
      best = off;
    }
  }
  if (d > 1u) {
    const std::uint32_t a = best >= d - 1u ? best - (d - 1u) : 0u;
    const std::uint32_t b = best + (d - 1u) <= range ? best + (d - 1u) : range;
    std::uint32_t fine = a;
    double fineScore = -1e300;
    for (std::uint32_t off = a; off <= b; ++off) {
      const double v = score(off, 1u);
      if (v > fineScore) {
        fineScore = v;
        fine = off;
      }
    }
    best = fine;
  }
  return lo + static_cast<std::int64_t>(best);
}

}  // namespace

bool TimeStretchGetShape(std::uint32_t sampleRate, float maxSpeed, TimeStretchShape* out,
                         float windowMs, float searchMs) noexcept {
  if (out == nullptr) return false;
  return Shape(sampleRate, maxSpeed, windowMs, searchMs, out);
}

std::size_t TimeStretchStateBytes(std::uint32_t channels, std::uint32_t sampleRate, float maxSpeed,
                                  float windowMs, float searchMs) noexcept {
  Lay l;
  if (!MakeLayout(channels, sampleRate, maxSpeed, windowMs, searchMs, &l)) return 0;
  return l.bytes;
}

bool TimeStretchInit(void* state, std::size_t bytes, std::uint32_t channels,
                     std::uint32_t sampleRate, float maxSpeed, float windowMs,
                     float searchMs) noexcept {
  Lay l;
  if (state == nullptr || !MakeLayout(channels, sampleRate, maxSpeed, windowMs, searchMs, &l))
    return false;
  if (bytes < l.bytes) return false;
  std::memset(state, 0, l.bytes);
  TsState& s = St(state);
  s.channels = channels;
  s.rate = sampleRate;
  s.hop = l.s.hopFrames;
  s.search = l.s.searchFrames;
  s.dec = l.s.decimation;
  s.look = l.s.lookaheadFrames;
  s.pad = l.s.padFrames;
  s.maxSpeed = ClampMs(maxSpeed, 1.0f, kTimeStretchMaxSpeed, kTimeStretchMaxSpeed);
  s.windowMs = windowMs;
  s.searchMs = searchMs;
  s.winOff = l.winOff;
  s.histOff = l.histOff;
  s.monoOff = l.monoOff;
  s.bytes = static_cast<std::uint32_t>(l.bytes);
  float* w = detail::At<float>(state, s.winOff);
  for (std::uint32_t j = 0; j < s.hop; ++j) {
    const double a = std::sin(detail::kPi * (static_cast<double>(j) + 0.5) / (2.0 * s.hop));
    w[j] = static_cast<float>(a * a);
  }
  ResetDynamic(s);
  s.magic = kTsMagic;
  return true;
}

void TimeStretchReset(void* state) noexcept {
  TsState& s = St(state);
  ResetDynamic(s);
  std::memset(detail::At<float>(state, s.histOff), 0,
              sizeof(float) * std::size_t{s.channels} * s.pad);
}

std::uint32_t TimeStretchChannels(const void* state) noexcept { return St(state).channels; }
std::uint32_t TimeStretchPadFrames(const void* state) noexcept { return St(state).pad; }
std::uint32_t TimeStretchHopFrames(const void* state) noexcept { return St(state).hop; }
std::uint32_t TimeStretchLookaheadFrames(const void* state) noexcept { return St(state).look; }

std::uint32_t TimeStretchMaxInputFrames(const void* state, std::uint32_t outFrames) noexcept {
  const TsState& s = St(state);
  const std::uint64_t hops = (std::uint64_t{outFrames} + s.hop - 1u) / s.hop + 1u;
  const std::uint64_t adv =
      static_cast<std::uint64_t>(std::ceil(static_cast<double>(s.maxSpeed) * s.hop)) + 1u;
  const std::uint64_t n = hops * adv + s.look;
  return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(n);
}

std::uint32_t TimeStretchInputFrames(const void* state, std::uint32_t outFrames, float speedStart,
                                     float speedEnd) noexcept {
  const TsState& s = St(state);
  const std::int64_t req = Requirement(s, outFrames, speedStart, speedEnd);
  return static_cast<std::uint32_t>(req - s.end);
}

std::uint32_t TimeStretchProcess(void* state, float* const* in, std::uint32_t inFrames,
                                 float* const* out, std::uint32_t outFrames, float speedStart,
                                 float speedEnd) noexcept {
  TsState& s = St(state);
  const std::uint32_t need =
      static_cast<std::uint32_t>(Requirement(s, outFrames, speedStart, speedEnd) - s.end);
  if (inFrames < need) {
    for (std::uint32_t c = 0; c < s.channels; ++c) Clear(out[c], outFrames);
    return 0;
  }
  const std::uint32_t pad = s.pad;
  const std::int64_t base = s.end - static_cast<std::int64_t>(pad);
  float* hist = detail::At<float>(state, s.histOff);
  for (std::uint32_t c = 0; c < s.channels; ++c)
    std::memcpy(in[c], hist + std::size_t{c} * pad, sizeof(float) * pad);
  const float* wr = detail::At<float>(state, s.winOff);
  const std::uint32_t hop = s.hop;
  for (std::uint32_t o = 0; o < outFrames;) {
    if (s.started == 0u || s.h >= hop) {
      if (s.started == 0u) {
        s.started = 1u;
        s.tau = 0;
        s.pPrev = -static_cast<std::int64_t>(hop);
        s.pCur = 0;
      } else {
        s.tau += Increment(s, SpeedAt(s, speedStart, speedEnd, o, outFrames));
        const std::int64_t natural = s.pCur + hop;
        const std::int64_t t = Nominal(s.tau);
        // Never before the first input frame (the history before it is silence).
        const std::int64_t lo = t - s.search > 0 ? t - s.search : 0, hi = t + s.search;
        s.pPrev = s.pCur;
        s.pCur = natural >= lo && natural <= hi ? natural : Search(s, in, base, natural, lo, hi);
      }
      s.h = 0;
    }
    const std::uint32_t len = hop - s.h < outFrames - o ? hop - s.h : outFrames - o;
    const bool natural = s.pCur == s.pPrev + hop;
    for (std::uint32_t c = 0; c < s.channels; ++c) {
      const float* cur = in[c] + (s.pCur + s.h - base);
      float* y = out[c] + o;
      if (natural) {
        std::memcpy(y, cur, sizeof(float) * len);
      } else {
        const float* prev = in[c] + (s.pPrev + hop + s.h - base);
        for (std::uint32_t j = 0; j < len; ++j) {
          const std::uint32_t k = s.h + j;
          y[j] = wr[hop - 1u - k] * prev[j] + wr[k] * cur[j];
        }
      }
    }
    s.h += len;
    o += len;
  }
  s.end += need;
  for (std::uint32_t c = 0; c < s.channels; ++c)
    std::memcpy(hist + std::size_t{c} * pad, in[c] + need, sizeof(float) * pad);
  return need;
}

std::uint64_t TimeStretchConsumedFrames(const void* state) noexcept {
  return static_cast<std::uint64_t>(St(state).end);
}

double TimeStretchPendingOffset(const void* state) noexcept {
  const TsState& s = St(state);
  if (s.started == 0u) return 0.0;
  const std::int64_t next = s.h >= s.hop ? s.pCur + s.hop : s.pCur + s.h;
  return static_cast<double>(next - s.end);
}

}  // namespace snd::dsp
