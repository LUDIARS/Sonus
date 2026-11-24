// Convolution reverb (convolution.hpp): prepared IRs, the IR registry and the
// standard effect snd.fx.convolution (EffectKind::Convolution).
#include "snd/dsp/convolution.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

#include "fx_common.hpp"
#include "snd/dsp/fft.hpp"
#include "snd/platform/simd.hpp"

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "IR blobs are read as little endian");
#endif

namespace snd::dsp {

// The prepared IR. Offsets are relative to the start (position independent).
struct ConvolutionIr {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t channels;
  std::uint32_t frames;      // at the target rate
  std::uint32_t partition;   // B
  std::uint32_t partitions;  // P = ceil(frames / B)
  std::uint32_t sampleRate;  // target rate
  std::uint32_t bytes;
  std::uint32_t headOff;     // channels x B floats: taps 0..B-1 (time domain)
  std::uint32_t specOff;     // channels x P x 2B floats: per partition re[B], im[B] (scaled 1/(2B))
  std::uint32_t planOff;     // FFT plan for 2B (used while preparing)
  std::uint32_t workOff;     // 4B floats scratch (used while preparing)
  std::uint32_t pad[4];
};
static_assert(sizeof(ConvolutionIr) == 64);

namespace {

namespace simd = snd::simd;
using simd::F32x8;

constexpr std::uint32_t kIrMagic = 0x50494353u;  // 'SCIP'
constexpr std::uint32_t kIrVersion = 1;
constexpr std::size_t kMaxStateBytes = std::size_t{1} << 30;
constexpr std::uint32_t kResampleZeros = 16;  // zero crossings on each side

// Writers reserve a starting position and scan consecutive entries locally. Only
// the writer that claimed ir can update lastTicket, before publishing number.
struct Entry {
  std::atomic<std::uint32_t> number;
  std::atomic<const ConvolutionIr*> ir;
  std::uint64_t lastTicket;
};
Entry g_entries[kConvolutionMaxIrSlots];
std::atomic<std::uint64_t> g_cursor{1};

bool IsPow2(std::uint32_t v) noexcept { return v != 0u && (v & (v - 1u)) == 0u; }

std::uint32_t NextPow2(std::uint32_t v) noexcept {
  std::uint32_t p = 1u;
  while (p < v && p < 0x80000000u) p <<= 1u;
  return p;
}

bool ValidIr(const ConvolutionIr* ir) noexcept {
  return ir != nullptr && ir->magic == kIrMagic && ir->version == kIrVersion;
}

// Partition rule (parameter PartitionFrames, see convolution.hpp).
std::uint32_t PartitionFor(float param, std::uint32_t maxFrames) noexcept {
  const std::uint32_t v = param > 0.0f ? static_cast<std::uint32_t>(param) : 0u;
  if (v == 0u) {
    const std::uint32_t p = NextPow2(maxFrames == 0u ? 1u : maxFrames);
    return p < kConvolutionMinPartition ? kConvolutionMinPartition : (p > 1024u ? 1024u : p);
  }
  const std::uint32_t p = NextPow2(v);
  return p < kConvolutionMinPartition ? kConvolutionMinPartition
                                      : (p > kConvolutionMaxPartition ? kConvolutionMaxPartition : p);
}

// ---------------------------------------------------------------------------
// IR preparation
// ---------------------------------------------------------------------------
struct Samples {
  const float* const* ch;     // or
  const unsigned char* blob;  // little-endian f32, channel after channel
  std::uint32_t frames;
  float Get(std::uint32_t c, std::uint32_t k) const noexcept {
    if (ch != nullptr) return ch[c][k];
    float v;
    std::memcpy(&v, blob + (std::size_t{c} * frames + k) * 4u, 4);
    return v;
  }
};

struct IrLayout {
  std::uint32_t headOff, specOff, planOff, workOff, frames, partitions;
  std::size_t bytes;
};

bool MakeIrLayout(std::uint32_t channels, std::uint32_t frames, std::uint32_t srcRate,
                  std::uint32_t targetRate, std::uint32_t b, IrLayout* l) noexcept {
  if (channels == 0u || channels > kConvolutionMaxIrChannels || frames == 0u) return false;
  if (targetRate == 0u || targetRate > kConvolutionMaxRate || srcRate > kConvolutionMaxRate)
    return false;
  if (!IsPow2(b) || b < kConvolutionMinPartition || b > kConvolutionMaxPartition) return false;
  const std::uint32_t tf = ConvolutionIrTargetFrames(frames, srcRate, targetRate);
  if (tf == 0u) return false;
  l->frames = tf;
  l->partitions = (tf + b - 1u) / b;
  detail::Carver c(nullptr, sizeof(ConvolutionIr));
  l->headOff = c.Take<float>(std::size_t{channels} * b);
  l->specOff = c.Take<float>(std::size_t{channels} * l->partitions * 2u * b);
  l->planOff = c.Take<unsigned char>(FftPlanBytes(2u * b));
  l->workOff = c.Take<float>(4u * std::size_t{b});
  l->bytes = c.Size();
  return l->bytes < 0xFFFFFFFFu;
}

double Blackman(double x) noexcept {  // x in [-1, 1]
  return 0.42 + 0.5 * std::cos(detail::kPi * x) + 0.08 * std::cos(2.0 * detail::kPi * x);
}

// Target-rate sample j of channel c (windowed-sinc resampling when the rates differ).
float TargetSample(const Samples& s, std::uint32_t c, std::uint32_t j, std::uint32_t srcRate,
                   std::uint32_t targetRate) noexcept {
  if (srcRate == targetRate) return j < s.frames ? s.Get(c, j) : 0.0f;
  const double ratio = static_cast<double>(srcRate) / static_cast<double>(targetRate);
  const double fc = targetRate < srcRate ? static_cast<double>(targetRate) / srcRate : 1.0;
  const double half = static_cast<double>(kResampleZeros) / fc;
  const double t = static_cast<double>(j) * ratio;
  double k0 = std::ceil(t - half), k1 = std::floor(t + half);
  if (k0 < 0.0) k0 = 0.0;
  if (k1 > static_cast<double>(s.frames) - 1.0) k1 = static_cast<double>(s.frames) - 1.0;
  double acc = 0.0;
  for (double k = k0; k <= k1; k += 1.0) {
    const double u = t - k;
    const double x = fc * u;
    const double sinc = x == 0.0 ? 1.0 : std::sin(detail::kPi * x) / (detail::kPi * x);
    acc += static_cast<double>(s.Get(c, static_cast<std::uint32_t>(k))) * fc * sinc *
           Blackman(u / half);
  }
  return static_cast<float>(acc);
}

const ConvolutionIr* Prepare(void* mem, std::size_t bytes, const Samples& src,
                             std::uint32_t channels, std::uint32_t srcRate, float gain,
                             std::uint32_t targetRate, std::uint32_t b) noexcept {
  if (srcRate == 0u) srcRate = targetRate;
  IrLayout l;
  if (mem == nullptr || !MakeIrLayout(channels, src.frames, srcRate, targetRate, b, &l)) return nullptr;
  if (bytes < l.bytes || !(gain == gain) || std::isinf(gain)) return nullptr;
  for (std::uint32_t c = 0; c < channels; ++c)  // D-219: no NaN / Inf samples
    for (std::uint32_t k = 0; k < src.frames; ++k)
      if (!std::isfinite(src.Get(c, k))) return nullptr;
  std::memset(mem, 0, l.bytes);
  ConvolutionIr& ir = *static_cast<ConvolutionIr*>(mem);
  ir.channels = channels;
  ir.frames = l.frames;
  ir.partition = b;
  ir.partitions = l.partitions;
  ir.sampleRate = targetRate;
  ir.bytes = static_cast<std::uint32_t>(l.bytes);
  ir.headOff = l.headOff;
  ir.specOff = l.specOff;
  ir.planOff = l.planOff;
  ir.workOff = l.workOff;
  void* plan = detail::At<unsigned char>(mem, l.planOff);
  if (!FftPlanInit(plan, FftPlanBytes(2u * b), 2u * b)) return nullptr;
  float* time = detail::At<float>(mem, l.workOff);
  float* work = time + 2u * b;
  const float scale = gain / static_cast<float>(2u * b);
  for (std::uint32_t c = 0; c < channels; ++c) {
    float* head = detail::At<float>(mem, l.headOff) + std::size_t{c} * b;
    for (std::uint32_t p = 0; p < l.partitions; ++p) {
      for (std::uint32_t k = 0; k < b; ++k) {
        const std::uint32_t j = p * b + k;
        time[k] = j < l.frames ? TargetSample(src, c, j, srcRate, targetRate) : 0.0f;
      }
      for (std::uint32_t k = b; k < 2u * b; ++k) time[k] = 0.0f;
      if (p == 0u)
        for (std::uint32_t k = 0; k < b; ++k) head[k] = time[k] * gain;
      float* re = detail::At<float>(mem, l.specOff) + (std::size_t{c} * l.partitions + p) * 2u * b;
      float* im = re + b;
      FftRealForward(plan, time, re, im, work);
      for (std::uint32_t k = 0; k < b; ++k) {
        re[k] *= scale;
        im[k] *= scale;
      }
    }
  }
  ir.version = kIrVersion;
  ir.magic = kIrMagic;
  return &ir;
}

// ---------------------------------------------------------------------------
// Effect instance
// ---------------------------------------------------------------------------
// Routing of output o: inputs and IR channels.
enum class Route : std::uint32_t { PerChannel, Matrix, Modulo };

struct ConvState {
  fx::FxHeader h;
  std::uint32_t b;        // partition
  std::uint32_t cap;      // FDL slots
  std::uint32_t zl;       // zero latency (direct head)
  std::uint32_t chans;
  std::uint32_t f;        // fill position in the current period, 0..b-1
  std::uint32_t w;        // FDL slot written last
  std::uint32_t valid;    // FDL slots holding real input since the last wake-up
  std::uint32_t dormant;  // tail done: delay line invalid, silent input is skipped
  std::uint32_t active;   // IR frames used by the last call
  std::uint32_t planOff;
  std::uint32_t inOff;    // chans x (2b + 8): input of the previous and current period
  std::uint32_t fdlOff;   // chans x cap x 2b: input spectra
  std::uint32_t outOff;   // chans x b: FDL output for the current period
  std::uint32_t accOff;   // 2b: spectrum accumulator
  std::uint32_t timeOff;  // 2b: inverse FFT output
  std::uint32_t workOff;  // 2b: FFT scratch
  std::uint32_t tmpOff;   // b + 8: wet of one output channel (new side)
  // D-218: IR switch with a cross-fade; the IR's direct-head taps are copied into the
  // state, so the old IR is never read once a switch has been seen.
  std::uint32_t headCh;   // head taps kept per bank: min(16, chans^2)
  std::uint32_t headOff;  // 2 banks x headCh x b floats
  std::uint32_t newOff;   // chans x b: FDL output of the new IR during a fade
  std::uint32_t oldTmpOff;  // b + 8: wet of one output channel (old side)
  std::uint32_t curNum;   // IR number in use (0 = none)
  std::uint32_t curBank;  // head bank of the IR in use
  std::uint32_t curRoute, curIrCh;
  std::uint32_t oldRoute, oldIrCh;
  std::uint32_t fading;   // 1 while cross-fading from the old IR (until the period ends)
  std::uint32_t fadeFrom; // period position the fade started at
  std::uint32_t pad[3];
};
static_assert(sizeof(ConvState) % 64 == 0);

std::uint32_t HeadChannels(std::uint32_t chans) noexcept {
  const std::uint32_t sq = chans * chans;
  return sq < kConvolutionMaxIrChannels ? sq : kConvolutionMaxIrChannels;
}

std::size_t LayoutFor(ConvState* s, const fx::FxConfig& cfg) noexcept {
  const std::uint32_t b = PartitionFor(cfg.init[kConvPartitionFrames], cfg.maxFrames);
  const std::uint32_t ch = cfg.channels;
  const double ms = static_cast<double>(cfg.init[kConvMaxIrMs]);
  double frames = std::ceil(ms * 0.001 * static_cast<double>(cfg.sizingRate));
  if (frames < 1.0) frames = 1.0;
  std::uint64_t cap = (static_cast<std::uint64_t>(frames) + b - 1u) / b;
  const std::uint64_t capMax = kMaxStateBytes / (8ull * b * ch);
  if (cap > capMax) cap = capMax;
  if (cap < 1u) cap = 1u;
  const std::uint32_t hc = HeadChannels(ch);
  detail::Carver c(s, sizeof(ConvState));
  const std::uint32_t planOff = c.Take<unsigned char>(FftPlanBytes(2u * b));
  const std::uint32_t inOff = c.Take<float>(std::size_t{ch} * (2u * b + 8u));
  const std::uint32_t fdlOff = c.Take<float>(std::size_t{ch} * cap * 2u * b);
  const std::uint32_t outOff = c.Take<float>(std::size_t{ch} * b);
  const std::uint32_t accOff = c.Take<float>(2u * b);
  const std::uint32_t timeOff = c.Take<float>(2u * b);
  const std::uint32_t workOff = c.Take<float>(2u * b);
  const std::uint32_t tmpOff = c.Take<float>(b + 8u);
  const std::uint32_t headOff = c.Take<float>(2u * std::size_t{hc} * b);
  const std::uint32_t newOff = c.Take<float>(std::size_t{ch} * b);
  const std::uint32_t oldTmpOff = c.Take<float>(b + 8u);
  if (s != nullptr) {
    s->b = b;
    s->cap = static_cast<std::uint32_t>(cap);
    s->zl = cfg.init[kConvZeroLatency] != 0.0f ? 1u : 0u;
    s->chans = ch;
    s->planOff = planOff;
    s->inOff = inOff;
    s->fdlOff = fdlOff;
    s->outOff = outOff;
    s->accOff = accOff;
    s->timeOff = timeOff;
    s->workOff = workOff;
    s->tmpOff = tmpOff;
    s->headCh = hc;
    s->headOff = headOff;
    s->newOff = newOff;
    s->oldTmpOff = oldTmpOff;
  }
  return c.Size();
}

float* InBuf(ConvState& s, std::uint32_t c) noexcept {
  return detail::At<float>(&s, s.inOff) + std::size_t{c} * (2u * s.b + 8u);
}
float* Fdl(ConvState& s, std::uint32_t c, std::uint32_t slot) noexcept {
  return detail::At<float>(&s, s.fdlOff) + (std::size_t{c} * s.cap + slot) * 2u * s.b;
}
float* OutBuf(ConvState& s, std::uint32_t c) noexcept {
  return detail::At<float>(&s, s.outOff) + std::size_t{c} * s.b;
}
float* NewBuf(ConvState& s, std::uint32_t c) noexcept {
  return detail::At<float>(&s, s.newOff) + std::size_t{c} * s.b;
}
float* HeadTaps(ConvState& s, std::uint32_t bank, std::uint32_t irCh) noexcept {
  return detail::At<float>(&s, s.headOff) + (std::size_t{bank} * s.headCh + irCh) * s.b;
}

struct ActiveIr {
  const ConvolutionIr* ir;  // nullptr: no usable IR
  std::uint32_t number;     // registry number when ir != nullptr, else 0
  Route route;
  std::uint32_t engine;  // FDL partitions used
  std::uint32_t first;   // IR partition of FDL slot 0 (1 with the direct head, else 0)
};

ActiveIr Resolve(const ConvState& s, std::uint32_t number) noexcept {
  ActiveIr a{nullptr, 0u, Route::PerChannel, 0u, 0u};
  const ConvolutionIr* ir = ConvolutionRegisteredIr(number);
  if (!ValidIr(ir) || ir->partition != s.b || ir->sampleRate != s.h.sampleRate) return a;
  a.ir = ir;
  a.number = number;
  const std::uint32_t c = s.chans, n = ir->channels;
  a.route = n == c ? Route::PerChannel : (c >= 2u && n == c * c ? Route::Matrix : Route::Modulo);
  a.first = s.zl != 0u ? 1u : 0u;
  const std::uint32_t parts = ir->partitions > a.first ? ir->partitions - a.first : 0u;
  a.engine = parts < s.cap ? parts : s.cap;
  return a;
}

std::uint32_t IrChannel(Route route, std::uint32_t irChannels, std::uint32_t chans,
                        std::uint32_t in, std::uint32_t o) noexcept {
  switch (route) {
    case Route::PerChannel: return o;
    case Route::Matrix: return in * chans + o;
    case Route::Modulo: break;
  }
  return o % irChannels;
}

const float* Spectrum(const ActiveIr& a, std::uint32_t irCh, std::uint32_t part) noexcept {
  const ConvolutionIr& ir = *a.ir;
  return detail::At<float>(&ir, ir.specOff) +
         (std::size_t{irCh} * ir.partitions + part) * 2u * ir.partition;
}

// acc += x * h over the packed split spectra (bin 0: DC and Nyquist, real).
void MacSpectrum(float* acc, const float* x, const float* hsp, std::uint32_t b, float* dc,
                 float* ny) noexcept {
  float* ar = acc;
  float* ai = acc + b;
  const float* xr = x;
  const float* xi = x + b;
  const float* hr = hsp;
  const float* hi = hsp + b;
  for (std::uint32_t k = 0; k < b; k += 8u) {
    const F32x8 a = F32x8::LoadU(xr + k), bb = F32x8::LoadU(xi + k);
    const F32x8 c = F32x8::LoadU(hr + k), d = F32x8::LoadU(hi + k);
    F32x8 r = F32x8::LoadU(ar + k), i = F32x8::LoadU(ai + k);
    r = simd::MulAdd(a, c, r);
    r = simd::NegMulAdd(bb, d, r);
    i = simd::MulAdd(a, d, i);
    i = simd::MulAdd(bb, c, i);
    r.StoreU(ar + k);
    i.StoreU(ai + k);
  }
  *dc = *dc + xr[0] * hr[0];
  *ny = *ny + xi[0] * hi[0];
}

// FDL output of the period that starts now: sum over the valid slots (newest = s.w)
// of input spectrum x IR partition, inverse FFT, second half. `out(o)` = destination.
template <class Dst>
void Engine(ConvState& s, const ActiveIr& a, Dst out) noexcept {
  const std::uint32_t b = s.b;
  const void* plan = detail::At<unsigned char>(&s, s.planOff);
  float* work = detail::At<float>(&s, s.workOff);
  const std::uint32_t n = a.ir != nullptr ? (s.valid < a.engine ? s.valid : a.engine) : 0u;
  float* acc = detail::At<float>(&s, s.accOff);
  float* time = detail::At<float>(&s, s.timeOff);
  for (std::uint32_t o = 0; o < s.chans; ++o) {
    float* y = out(o);
    if (n == 0u) {
      std::memset(y, 0, sizeof(float) * b);
      continue;
    }
    std::memset(acc, 0, sizeof(float) * 2u * b);
    float dc = 0.0f, ny = 0.0f;
    for (std::uint32_t p = 0; p < n; ++p) {
      const std::uint32_t fs = s.w >= p ? s.w - p : s.w + s.cap - p;
      if (a.route == Route::Matrix) {
        for (std::uint32_t i = 0; i < s.chans; ++i)
          MacSpectrum(acc, Fdl(s, i, fs),
                      Spectrum(a, IrChannel(a.route, a.ir->channels, s.chans, i, o), a.first + p),
                      b, &dc, &ny);
      } else {
        MacSpectrum(acc, Fdl(s, o, fs),
                    Spectrum(a, IrChannel(a.route, a.ir->channels, s.chans, o, o), a.first + p), b,
                    &dc, &ny);
      }
    }
    acc[0] = dc;
    acc[b] = ny;
    FftRealInverse(plan, acc, acc + b, time, work);
    std::memcpy(y, time + b, sizeof(float) * b);
  }
}

// End of a period: transform the input, run the FDL, prepare the next period's output.
void Step(ConvState& s, const ActiveIr& a) noexcept {
  const std::uint32_t b = s.b;
  const void* plan = detail::At<unsigned char>(&s, s.planOff);
  float* work = detail::At<float>(&s, s.workOff);
  const std::uint32_t slot = s.w + 1u >= s.cap ? 0u : s.w + 1u;
  for (std::uint32_t c = 0; c < s.chans; ++c) {
    float* x = Fdl(s, c, slot);
    FftRealForward(plan, InBuf(s, c), x, x + b, work);
  }
  s.w = slot;
  if (s.valid < s.cap) ++s.valid;
  Engine(s, a, [&](std::uint32_t o) noexcept { return OutBuf(s, o); });
  for (std::uint32_t c = 0; c < s.chans; ++c) {
    float* x = InBuf(s, c);
    std::memcpy(x, x + b, sizeof(float) * b);
  }
}

void GoDormant(ConvState& s) noexcept {
  for (std::uint32_t c = 0; c < s.chans; ++c) {
    std::memset(InBuf(s, c), 0, sizeof(float) * (2u * s.b + 8u));
    std::memset(OutBuf(s, c), 0, sizeof(float) * s.b);
  }
  s.valid = 0;
  s.dormant = 1;
  s.fading = 0;
}

// Makes `a` the IR in use: its head taps go to bank `bank` (the only read of the
// IR's head; afterwards the head runs from the copy).
void Adopt(ConvState& s, const ActiveIr& a, std::uint32_t bank) noexcept {
  s.curNum = a.number;
  s.curBank = bank;
  s.curRoute = static_cast<std::uint32_t>(a.route);
  s.curIrCh = a.ir != nullptr ? a.ir->channels : 0u;
  if (a.ir == nullptr) return;
  const std::uint32_t n = s.curIrCh < s.headCh ? s.curIrCh : s.headCh;
  const float* src = detail::At<float>(a.ir, a.ir->headOff);
  for (std::uint32_t c = 0; c < n; ++c)
    std::memcpy(HeadTaps(s, bank, c), src + std::size_t{c} * s.b, sizeof(float) * s.b);
}

// Direct head from the copied taps: dst[j] = sum_i sum_k head[k] x_i[B + f + j - k],
// groups of 8 outputs (each lane is one output frame: independent of the grouping).
void HeadFir(ConvState& s, std::uint32_t bank, std::uint32_t route, std::uint32_t irCh,
             std::uint32_t o, std::uint32_t len, float* dst) noexcept {
  const std::uint32_t b = s.b;
  const Route r = static_cast<Route>(route);
  for (std::uint32_t j0 = 0; j0 < len; j0 += 8u) {
    F32x8 acc = F32x8::Zero();
    const auto run = [&](std::uint32_t in, std::uint32_t ch) noexcept {
      const float* hk = HeadTaps(s, bank, ch);
      const float* x = InBuf(s, in) + b + s.f + j0;
      for (std::uint32_t k = 0; k < b; ++k) acc = simd::MulAdd(F32x8::Set1(hk[k]), F32x8::LoadU(x - k), acc);
    };
    if (r == Route::Matrix) {
      for (std::uint32_t i = 0; i < s.chans; ++i) run(i, IrChannel(r, irCh, s.chans, i, o));
    } else {
      run(o, IrChannel(r, irCh, s.chans, o, o));
    }
    acc.StoreU(dst + j0);
  }
}

// Wet of output o for frames [f, f + len) from engine output `eng` and the head of
// bank / route / irCh (no head when irCh == 0 or without the direct head).
void Wet(ConvState& s, const float* eng, std::uint32_t bank, std::uint32_t route,
         std::uint32_t irCh, std::uint32_t o, std::uint32_t len, float* dst) noexcept {
  if (irCh != 0u && s.zl != 0u) {
    HeadFir(s, bank, route, irCh, o, len, dst);
    for (std::uint32_t j = 0; j < len; ++j) dst[j] = eng[j] + dst[j];
  } else {
    std::memcpy(dst, eng, sizeof(float) * len);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: preparation, blobs, registry
// ---------------------------------------------------------------------------
std::uint32_t ConvolutionIrTargetFrames(std::uint32_t frames, std::uint32_t sampleRate,
                                        std::uint32_t targetRate) noexcept {
  if (frames == 0u || targetRate == 0u || targetRate > kConvolutionMaxRate ||
      sampleRate > kConvolutionMaxRate)
    return 0;
  if (sampleRate == 0u || sampleRate == targetRate)
    return frames <= kConvolutionMaxIrFrames ? frames : 0u;
  const std::uint64_t num = std::uint64_t{frames} * targetRate;
  const std::uint64_t tf = (num + sampleRate - 1u) / sampleRate;
  return tf <= kConvolutionMaxIrFrames ? static_cast<std::uint32_t>(tf) : 0u;
}

std::size_t ConvolutionIrBytes(const ConvolutionIrDesc& desc, std::uint32_t targetRate,
                               std::uint32_t partitionFrames) noexcept {
  IrLayout l;
  const std::uint32_t src = desc.sampleRate == 0u ? targetRate : desc.sampleRate;
  if (desc.channels == nullptr ||
      !MakeIrLayout(desc.channelCount, desc.frames, src, targetRate, partitionFrames, &l))
    return 0;
  return l.bytes;
}

const ConvolutionIr* ConvolutionPrepareIr(void* mem, std::size_t bytes,
                                          const ConvolutionIrDesc& desc, std::uint32_t targetRate,
                                          std::uint32_t partitionFrames) noexcept {
  if (desc.channels == nullptr) return nullptr;
  for (std::uint32_t c = 0; c < desc.channelCount && c < kConvolutionMaxIrChannels; ++c)
    if (desc.channels[c] == nullptr) return nullptr;
  const Samples s{desc.channels, nullptr, desc.frames};
  return Prepare(mem, bytes, s, desc.channelCount, desc.sampleRate, desc.gain, targetRate,
                 partitionFrames);
}

bool ConvolutionParseIrBlob(const void* blob, std::uint64_t blobBytes,
                            ConvolutionBlobInfo* out) noexcept {
  if (blob == nullptr || blobBytes < kConvolutionBlobHeaderBytes) return false;
  std::uint32_t hd[8];
  std::memcpy(hd, blob, sizeof(hd));
  float gain;
  std::memcpy(&gain, &hd[5], 4);
  if (hd[0] != kConvolutionBlobMagic || hd[1] != kConvolutionBlobVersion) return false;
  if (hd[2] == 0u || hd[2] > kConvolutionMaxIrChannels || hd[3] == 0u) return false;
  if (hd[4] == 0u || hd[4] > kConvolutionMaxRate || hd[6] != 0u || hd[7] != 0u) return false;
  if (!(gain == gain) || std::isinf(gain)) return false;
  if (blobBytes != kConvolutionBlobHeaderBytes + 4ull * hd[2] * hd[3]) return false;
  const unsigned char* smp = static_cast<const unsigned char*>(blob) + kConvolutionBlobHeaderBytes;
  for (std::uint64_t i = 0; i < std::uint64_t{hd[2]} * hd[3]; ++i) {  // D-219
    float v;
    std::memcpy(&v, smp + i * 4u, 4);
    if (!std::isfinite(v)) return false;
  }
  if (out != nullptr) {
    out->channels = hd[2];
    out->frames = hd[3];
    out->sampleRate = hd[4];
    out->gain = gain;
    out->samples = static_cast<const unsigned char*>(blob) + kConvolutionBlobHeaderBytes;
  }
  return true;
}

std::size_t ConvolutionIrBytesFromBlob(const void* blob, std::uint64_t blobBytes,
                                       std::uint32_t targetRate,
                                       std::uint32_t partitionFrames) noexcept {
  ConvolutionBlobInfo bi;
  IrLayout l;
  if (!ConvolutionParseIrBlob(blob, blobBytes, &bi) ||
      !MakeIrLayout(bi.channels, bi.frames, bi.sampleRate, targetRate, partitionFrames, &l))
    return 0;
  return l.bytes;
}

const ConvolutionIr* ConvolutionPrepareIrFromBlob(void* mem, std::size_t bytes, const void* blob,
                                                  std::uint64_t blobBytes,
                                                  std::uint32_t targetRate,
                                                  std::uint32_t partitionFrames) noexcept {
  ConvolutionBlobInfo bi;
  if (!ConvolutionParseIrBlob(blob, blobBytes, &bi)) return nullptr;
  const Samples s{nullptr, bi.samples, bi.frames};
  return Prepare(mem, bytes, s, bi.channels, bi.sampleRate, bi.gain, targetRate, partitionFrames);
}

std::size_t ConvolutionWriteIrBlob(void* dst, std::size_t bytes, const float* const* channels,
                                   std::uint32_t channelCount, std::uint32_t frames,
                                   std::uint32_t sampleRate, float gain) noexcept {
  const std::size_t need = kConvolutionBlobHeaderBytes + 4u * std::size_t{channelCount} * frames;
  if (dst == nullptr || channels == nullptr || bytes < need || channelCount == 0u ||
      channelCount > kConvolutionMaxIrChannels || frames == 0u)
    return 0;
  std::uint32_t hd[8] = {kConvolutionBlobMagic, kConvolutionBlobVersion, channelCount, frames,
                         sampleRate, 0u, 0u, 0u};
  std::memcpy(&hd[5], &gain, 4);
  unsigned char* p = static_cast<unsigned char*>(dst);
  std::memcpy(p, hd, sizeof(hd));
  for (std::uint32_t c = 0; c < channelCount; ++c)
    std::memcpy(p + kConvolutionBlobHeaderBytes + 4u * std::size_t{c} * frames, channels[c],
                4u * std::size_t{frames});
  return need;
}

std::uint32_t ConvolutionIrChannels(const ConvolutionIr* ir) noexcept {
  return ValidIr(ir) ? ir->channels : 0u;
}
std::uint32_t ConvolutionIrFrames(const ConvolutionIr* ir) noexcept {
  return ValidIr(ir) ? ir->frames : 0u;
}
std::uint32_t ConvolutionIrPartition(const ConvolutionIr* ir) noexcept {
  return ValidIr(ir) ? ir->partition : 0u;
}
std::uint32_t ConvolutionIrSampleRate(const ConvolutionIr* ir) noexcept {
  return ValidIr(ir) ? ir->sampleRate : 0u;
}

std::uint32_t ConvolutionRegisterIr(const ConvolutionIr* ir) noexcept {
  if (!ValidIr(ir)) return 0;
  constexpr auto slots = kConvolutionMaxIrSlots;
  const auto start = g_cursor.fetch_add(1u, std::memory_order_relaxed);
  for (std::uint32_t i = 0; i < slots; ++i) {
    auto ticket = start + i;
    Entry& e = g_entries[ticket & (slots - 1u)];
    const ConvolutionIr* expected = nullptr;
    if (!e.ir.compare_exchange_strong(expected, ir, std::memory_order_acq_rel)) continue;
    if (ticket <= e.lastTicket) ticket = e.lastTicket + slots;
    if ((ticket & kConvolutionMaxIrNumber) == 0u) ticket += slots;  // number zero is reserved
    e.lastTicket = ticket;
    const auto number = static_cast<std::uint32_t>(ticket & kConvolutionMaxIrNumber);
    e.number.store(number, std::memory_order_release);
    // In isolation the next registration starts just after this entry. Concurrent
    // registrars already own their starting positions and retain their full scans.
    if (ticket != start) {
      auto cursor = start + 1u;
      g_cursor.compare_exchange_strong(cursor, ticket + 1u, std::memory_order_relaxed);
    }
    return number;
  }
  return 0;
}

bool ConvolutionUnregisterIr(std::uint32_t number) noexcept {
  if (number == 0u || number > kConvolutionMaxIrNumber) return false;
  Entry& e = g_entries[number & (kConvolutionMaxIrSlots - 1u)];
  std::uint32_t expected = number;
  if (!e.number.compare_exchange_strong(expected, 0u, std::memory_order_acq_rel)) return false;
  e.ir.store(nullptr, std::memory_order_release);
  return true;
}

const ConvolutionIr* ConvolutionRegisteredIr(std::uint32_t number) noexcept {
  if (number == 0u || number > kConvolutionMaxIrNumber) return nullptr;
  const Entry& e = g_entries[number & (kConvolutionMaxIrSlots - 1u)];
  if (e.number.load(std::memory_order_acquire) != number) return nullptr;
  const ConvolutionIr* ir = e.ir.load(std::memory_order_acquire);
  if (e.number.load(std::memory_order_acquire) != number) return nullptr;
  return ir;
}

std::uint32_t ConvolutionPartitionFrames(const EffectDesc& desc) noexcept {
  if (desc.effect_type != kEffectConvolution || desc.max_frames == 0u) return 0;
  const EffectParamInfo& pi = GetEffectInfo(EffectKind::Convolution).params[kConvPartitionFrames];
  float v = pi.defaultValue;
  if (desc.initial_params != nullptr && desc.param_count > kConvPartitionFrames)
    v = desc.initial_params[kConvPartitionFrames];
  if (!(v == v)) v = pi.defaultValue;
  v = v < pi.minValue ? pi.minValue : (v > pi.maxValue ? pi.maxValue : v);
  return PartitionFor(std::nearbyint(v), desc.max_frames);
}

std::uint32_t ConvolutionStatePartition(const void* state) noexcept {
  return static_cast<const ConvState*>(state)->b;
}
// D-218 / D-214: the IR frames the instance can use (with the direct head: cap + 1 partitions).
std::uint32_t ConvolutionStateCapacityFrames(const void* state) noexcept {
  const ConvState& s = *static_cast<const ConvState*>(state);
  return (s.cap + s.zl) * s.b;
}
std::uint32_t ConvolutionStateActiveIrFrames(const void* state) noexcept {
  return static_cast<const ConvState*>(state)->active;
}

// ---------------------------------------------------------------------------
// Standard effect entry points (fx_common.hpp)
// ---------------------------------------------------------------------------
namespace fx {

std::size_t ConvolutionLayout(void* state, const FxConfig& cfg) noexcept {
  return LayoutFor(static_cast<ConvState*>(state), cfg);
}

void ConvolutionInit(void* state, const FxConfig& cfg) noexcept {
  ConvState& s = *static_cast<ConvState*>(state);
  (void)cfg;
  FftPlanInit(detail::At<unsigned char>(state, s.planOff), FftPlanBytes(2u * s.b), 2u * s.b);
  s.f = 0;
  s.w = 0;
  s.valid = 0;
  s.dormant = 1;
  s.active = 0;
  s.curNum = 0;
  s.curBank = 0;
  s.curRoute = s.oldRoute = 0;
  s.curIrCh = s.oldIrCh = 0;
  s.fading = 0;
  s.fadeFrom = 0;
  s.h.latency = s.zl != 0u ? 0u : s.b;
  const std::uint64_t tail = (std::uint64_t{s.cap} + 2u) * s.b + (s.zl != 0u ? 0u : s.b);
  s.h.tailFrames = tail > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<std::uint32_t>(tail);
}

// Same state as ConvolutionInit after Layout, without the full memset and without
// rebuilding the FFT plan (it does not change): O(C B). Not called by the effect
// framework yet; proposed for EffectReset / NaN recovery in requests/D-12.
void ConvolutionReset(void* state) noexcept {
  ConvState& s = *static_cast<ConvState*>(state);
  GoDormant(s);  // input history, FDL output, valid = 0 (the FDL is invalidated, not cleared)
  s.f = 0;
  s.w = 0;
  s.active = 0;
  s.curNum = 0;  // the next call adopts the IR at once (dormant)
  s.curBank = 0;
  s.curRoute = s.oldRoute = 0;
  s.curIrCh = s.oldIrCh = 0;
  s.fading = 0;
  s.fadeFrom = 0;
}

void ConvolutionProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  ConvState& s = *static_cast<ConvState*>(state);
  const std::uint32_t b = s.b;
  // D-218: IrSlot is the END value of the whole call. The framework writes it to
  // h.cur before it splits a call longer than max_frames into sub-blocks (LoadParams
  // in effects.cpp), so a sub-block never sees an interpolated number.
  const float slotParam = s.h.cur[kConvIrSlot];
  const std::uint32_t number =
      slotParam > 0.0f && slotParam <= static_cast<float>(kConvolutionMaxIrNumber)
          ? static_cast<std::uint32_t>(slotParam)
          : 0u;
  ActiveIr a = Resolve(s, number);
  if (a.number != s.curNum) {
    if (s.dormant != 0u) {
      Adopt(s, a, s.curBank);  // nothing is sounding: switch at once
    } else if (s.fading == 0u) {
      // Cross-fade from the switch to the end of the period: the old side is what was
      // already computed (OutBuf, head taps in the other bank), the new side is the
      // new IR on the same input history.
      s.oldRoute = s.curRoute;
      s.oldIrCh = s.curIrCh;
      Adopt(s, a, s.curBank ^ 1u);
      Engine(s, a, [&](std::uint32_t o) noexcept { return NewBuf(s, o); });
      s.fading = 1;
      s.fadeFrom = s.f;
    } else {
      a = Resolve(s, s.curNum);  // a fade is running: the next switch waits for its end
    }
  }
  s.active = a.ir == nullptr
                 ? 0u
                 : static_cast<std::uint32_t>(
                       detail::Min<std::uint64_t>(a.ir->frames, std::uint64_t{a.engine + a.first} * b));
  const bool rampDry = p.Ramps(kConvDryMix);
  const bool rampWet = p.Ramps(kConvWetMix) || p.Ramps(kConvWetGainDb);
  const float wetConst = p.s[kConvWetMix] * detail::DbToLin(p.s[kConvWetGainDb]);
  const float inv = 1.0f / static_cast<float>(io.frames);
  float* tmp = detail::At<float>(&s, s.tmpOff);
  float* oldTmp = detail::At<float>(&s, s.oldTmpOff);
  for (std::uint32_t done = 0; done < io.frames;) {
    const std::uint32_t len = io.frames - done < b - s.f ? io.frames - done : b - s.f;
    float peak = 0.0f;
    std::uint32_t loudEnd = 0;  // 1 + index of the last frame at or above the tail threshold
    for (std::uint32_t c = 0; c < s.chans; ++c) {
      float* dst = InBuf(s, c) + b + s.f;
      if (c < io.channels) {
        std::memcpy(dst, io.in[c] + done, sizeof(float) * len);
        for (std::uint32_t j = 0; j < len; ++j) {
          const float v = std::fabs(dst[j]);
          peak = v > peak ? v : peak;
          if (v >= detail::kTailSilence && j + 1u > loudEnd) loudEnd = j + 1u;
        }
      } else {
        std::memset(dst, 0, sizeof(float) * len);
      }
    }
    if (s.dormant != 0u) {
      if (peak == 0.0f) {
        for (std::uint32_t o = 0; o < io.channels; ++o) std::memset(io.out[o] + done, 0, sizeof(float) * len);
        fx::TailUpdate(s.h, 0.0f, len);
        s.f += len;
        if (s.f == b) s.f = 0;  // the input buffer is all zeros: nothing to shift
        done += len;
        continue;
      }
      s.dormant = 0;
      s.valid = 0;
    }
    for (std::uint32_t o = 0; o < io.channels; ++o) {
      const float* eng = (s.fading != 0u ? NewBuf(s, o) : OutBuf(s, o)) + s.f;
      Wet(s, eng, s.curBank, s.curRoute, s.curIrCh, o, len, tmp);
      if (s.fading != 0u) {
        Wet(s, OutBuf(s, o) + s.f, s.curBank ^ 1u, s.oldRoute, s.oldIrCh, o, len, oldTmp);
        const float span = static_cast<float>(b - s.fadeFrom);
        for (std::uint32_t j = 0; j < len; ++j) {
          const float g = static_cast<float>(s.f + j - s.fadeFrom + 1u) / span;
          tmp[j] = oldTmp[j] + g * (tmp[j] - oldTmp[j]);
        }
      }
      const float* dry = InBuf(s, o) + (s.zl != 0u ? b + s.f : s.f);
      float* y = io.out[o] + done;
      if (!rampDry && !rampWet) {
        const float gd = p.s[kConvDryMix];
        for (std::uint32_t j = 0; j < len; ++j) y[j] = gd * dry[j] + wetConst * tmp[j];
      } else {
        for (std::uint32_t j = 0; j < len; ++j) {
          const float t = static_cast<float>(done + j) * inv;
          const float gd = p.At(kConvDryMix, t);
          const float gw = rampWet ? p.At(kConvWetMix, t) * detail::DbToLin(p.At(kConvWetGainDb, t))
                                   : wetConst;
          y[j] = gd * dry[j] + gw * tmp[j];
        }
      }
    }
    // Tail bookkeeping exact to the frame (independent of how calls are split).
    if (loudEnd != 0u) {
      s.h.silentFrames = 0;
      s.h.cleared = 0;
    }
    fx::TailUpdate(s.h, 0.0f, len - loudEnd);
    s.f += len;
    if (s.f == b) {
      Step(s, a);
      s.f = 0;
      s.fading = 0;
    }
    if (fx::TailShouldClear(s.h)) GoDormant(s);
    done += len;
  }
}

}  // namespace fx

}  // namespace snd::dsp
