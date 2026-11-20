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

std::atomic<const ConvolutionIr*> g_slots[kConvolutionMaxIrSlots];

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
  std::uint32_t tmpOff;   // b + 8: wet of one output channel
  std::uint32_t pad[15];
};
static_assert(sizeof(ConvState) % 64 == 0);

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
  detail::Carver c(s, sizeof(ConvState));
  const std::uint32_t planOff = c.Take<unsigned char>(FftPlanBytes(2u * b));
  const std::uint32_t inOff = c.Take<float>(std::size_t{ch} * (2u * b + 8u));
  const std::uint32_t fdlOff = c.Take<float>(std::size_t{ch} * cap * 2u * b);
  const std::uint32_t outOff = c.Take<float>(std::size_t{ch} * b);
  const std::uint32_t accOff = c.Take<float>(2u * b);
  const std::uint32_t timeOff = c.Take<float>(2u * b);
  const std::uint32_t workOff = c.Take<float>(2u * b);
  const std::uint32_t tmpOff = c.Take<float>(b + 8u);
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

// Routing of output o: inputs and IR channels.
enum class Route : std::uint32_t { PerChannel, Matrix, Modulo };

struct ActiveIr {
  const ConvolutionIr* ir;
  Route route;
  std::uint32_t engine;  // FDL partitions used
  std::uint32_t first;   // IR partition of FDL slot 0 (1 with the direct head, else 0)
};

ActiveIr Resolve(const ConvState& s, float slotParam) noexcept {
  ActiveIr a{nullptr, Route::PerChannel, 0u, 0u};
  const std::uint32_t slot = slotParam > 0.0f ? static_cast<std::uint32_t>(slotParam) : 0u;
  const ConvolutionIr* ir = ConvolutionRegisteredIr(slot);
  if (!ValidIr(ir) || ir->partition != s.b || ir->sampleRate != s.h.sampleRate) return a;
  a.ir = ir;
  const std::uint32_t c = s.chans, n = ir->channels;
  a.route = n == c ? Route::PerChannel : (c >= 2u && n == c * c ? Route::Matrix : Route::Modulo);
  a.first = s.zl != 0u ? 1u : 0u;
  const std::uint32_t parts = ir->partitions > a.first ? ir->partitions - a.first : 0u;
  a.engine = parts < s.cap ? parts : s.cap;
  return a;
}

std::uint32_t IrChannel(const ActiveIr& a, std::uint32_t chans, std::uint32_t in,
                        std::uint32_t o) noexcept {
  switch (a.route) {
    case Route::PerChannel: return o;
    case Route::Matrix: return in * chans + o;
    case Route::Modulo: break;
  }
  return o % a.ir->channels;
}

const float* Spectrum(const ActiveIr& a, std::uint32_t irCh, std::uint32_t part) noexcept {
  const ConvolutionIr& ir = *a.ir;
  return detail::At<float>(&ir, ir.specOff) +
         (std::size_t{irCh} * ir.partitions + part) * 2u * ir.partition;
}
const float* Head(const ActiveIr& a, std::uint32_t irCh) noexcept {
  const ConvolutionIr& ir = *a.ir;
  return detail::At<float>(&ir, ir.headOff) + std::size_t{irCh} * ir.partition;
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
  const std::uint32_t n = a.ir != nullptr ? (s.valid < a.engine ? s.valid : a.engine) : 0u;
  float* acc = detail::At<float>(&s, s.accOff);
  float* time = detail::At<float>(&s, s.timeOff);
  for (std::uint32_t o = 0; o < s.chans; ++o) {
    float* y = OutBuf(s, o);
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
          MacSpectrum(acc, Fdl(s, i, fs), Spectrum(a, IrChannel(a, s.chans, i, o), a.first + p), b,
                      &dc, &ny);
      } else {
        MacSpectrum(acc, Fdl(s, o, fs), Spectrum(a, IrChannel(a, s.chans, o, o), a.first + p), b,
                    &dc, &ny);
      }
    }
    acc[0] = dc;
    acc[b] = ny;
    FftRealInverse(plan, acc, acc + b, time, work);
    std::memcpy(y, time + b, sizeof(float) * b);
  }
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
}

// Direct head: tmp[j] += sum_i sum_k head_i[k] x_i[B + f + j - k], groups of 8 outputs
// (each lane is one output frame: the result does not depend on the grouping).
void HeadFir(ConvState& s, const ActiveIr& a, std::uint32_t o, std::uint32_t len,
             float* head8) noexcept {
  const std::uint32_t b = s.b;
  for (std::uint32_t j0 = 0; j0 < len; j0 += 8u) {
    F32x8 acc = F32x8::Zero();
    const auto run = [&](std::uint32_t in, std::uint32_t irCh) noexcept {
      const float* hk = Head(a, irCh);
      const float* x = InBuf(s, in) + b + s.f + j0;
      for (std::uint32_t k = 0; k < b; ++k) acc = simd::MulAdd(F32x8::Set1(hk[k]), F32x8::LoadU(x - k), acc);
    };
    if (a.route == Route::Matrix) {
      for (std::uint32_t i = 0; i < s.chans; ++i) run(i, IrChannel(a, s.chans, i, o));
    } else {
      run(o, IrChannel(a, s.chans, o, o));
    }
    acc.StoreU(head8 + j0);
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
  for (std::uint32_t i = 0; i < kConvolutionMaxIrSlots; ++i) {
    const ConvolutionIr* expected = nullptr;
    if (g_slots[i].compare_exchange_strong(expected, ir, std::memory_order_acq_rel)) return i + 1u;
  }
  return 0;
}

bool ConvolutionUnregisterIr(std::uint32_t slot) noexcept {
  if (slot == 0u || slot > kConvolutionMaxIrSlots) return false;
  return g_slots[slot - 1u].exchange(nullptr, std::memory_order_acq_rel) != nullptr;
}

const ConvolutionIr* ConvolutionRegisteredIr(std::uint32_t slot) noexcept {
  if (slot == 0u || slot > kConvolutionMaxIrSlots) return nullptr;
  return g_slots[slot - 1u].load(std::memory_order_acquire);
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
std::uint32_t ConvolutionStateCapacityFrames(const void* state) noexcept {
  const ConvState& s = *static_cast<const ConvState*>(state);
  return s.cap * s.b;
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
  s.h.latency = s.zl != 0u ? 0u : s.b;
  const std::uint64_t tail = (std::uint64_t{s.cap} + 2u) * s.b + (s.zl != 0u ? 0u : s.b);
  s.h.tailFrames = tail > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<std::uint32_t>(tail);
}

void ConvolutionReset(void* state) noexcept {
  ConvState& s = *static_cast<ConvState*>(state);
  GoDormant(s);
  s.f = 0;
  s.w = 0;
}

void ConvolutionProcess(void* state, const FxIo& io, const FxParams& p) noexcept {
  ConvState& s = *static_cast<ConvState*>(state);
  const std::uint32_t b = s.b;
  const ActiveIr a = Resolve(s, p.e[kConvIrSlot]);
  s.active = a.ir == nullptr
                 ? 0u
                 : static_cast<std::uint32_t>(
                       detail::Min<std::uint64_t>(a.ir->frames, std::uint64_t{a.engine + a.first} * b));
  const bool rampDry = p.Ramps(kConvDryMix);
  const bool rampWet = p.Ramps(kConvWetMix) || p.Ramps(kConvWetGainDb);
  const float wetConst = p.s[kConvWetMix] * detail::DbToLin(p.s[kConvWetGainDb]);
  const float inv = 1.0f / static_cast<float>(io.frames);
  float* tmp = detail::At<float>(&s, s.tmpOff);
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
      const float* eng = OutBuf(s, o) + s.f;
      if (a.ir != nullptr && s.zl != 0u) {
        HeadFir(s, a, o, len, tmp);
        for (std::uint32_t j = 0; j < len; ++j) tmp[j] = eng[j] + tmp[j];
      } else {
        std::memcpy(tmp, eng, sizeof(float) * len);
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
    }
    if (fx::TailShouldClear(s.h)) GoDormant(s);
    done += len;
  }
}

}  // namespace fx

}  // namespace snd::dsp
