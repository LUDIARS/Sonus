#include "snd/dsp/meter.hpp"

#include <cmath>
#include <cstring>

#include "detail.hpp"
#include "snd/dsp/pan.hpp"

namespace snd::dsp {

// ---------------------------------------------------------------------------
// Peak
// ---------------------------------------------------------------------------
void PeakMeterInit(PeakMeterState& s, std::uint32_t channels, float releaseDbPerSecond,
                   std::uint32_t sampleRate) noexcept {
  std::memset(&s, 0, sizeof(s));
  s.channels = channels > kMaxChannels ? kMaxChannels : channels;
  if (releaseDbPerSecond > 0.0f && sampleRate > 0) {
    s.releasePerFrame = std::pow(10.0f, -releaseDbPerSecond / (20.0f * static_cast<float>(sampleRate)));
  } else {
    s.releasePerFrame = 1.0f;
  }
}

void PeakMeterProcess(PeakMeterState& s, const float* const* in, std::uint32_t frames) noexcept {
  if (frames == 0) return;
  const float decay = std::pow(s.releasePerFrame, static_cast<float>(frames));
  for (std::uint32_t c = 0; c < s.channels; ++c) {
    const float p = Peak(in[c], frames);
    float held = s.peak[c] * decay;
    if (held < detail::kFlushThreshold) held = 0.0f;
    s.peak[c] = p > held ? p : held;
  }
}

// ---------------------------------------------------------------------------
// RMS
// ---------------------------------------------------------------------------
void RmsMeterInit(RmsMeterState& s, std::uint32_t channels, float windowSeconds,
                  std::uint32_t sampleRate) noexcept {
  std::memset(&s, 0, sizeof(s));
  s.channels = channels > kMaxChannels ? kMaxChannels : channels;
  const double t = windowSeconds > 0.0f ? windowSeconds : 0.3;
  s.coef = std::exp(-1.0 / (t * static_cast<double>(sampleRate > 0 ? sampleRate : 48000)));
}

void RmsMeterProcess(RmsMeterState& s, const float* const* in, std::uint32_t frames) noexcept {
  const double a = s.coef, b = 1.0 - s.coef;
  for (std::uint32_t c = 0; c < s.channels; ++c) {
    double m = s.meanSquare[c];
    const float* x = in[c];
    for (std::uint32_t i = 0; i < frames; ++i) {
      const double v = x[i];
      m = a * m + b * v * v;
    }
    s.meanSquare[c] = m < 1e-30 ? 0.0 : m;
  }
}

float RmsMeterValue(const RmsMeterState& s, std::uint32_t channel) noexcept {
  if (channel >= s.channels) return 0.0f;
  return static_cast<float>(std::sqrt(s.meanSquare[channel]));
}

// ---------------------------------------------------------------------------
// Loudness (BS.1770-4)
// ---------------------------------------------------------------------------
namespace {

constexpr double kAbsGate = -70.0;
constexpr double kBinWidth = 0.05;

double EnergyToLufs(double e) noexcept {
  if (!(e > 0.0)) return kLoudnessSilence;
  return -0.691 + 10.0 * std::log10(e);
}

void KWeighting(double fs, float* shelf, float* hp) noexcept {
  // Stage 1: high shelf (analogue prototype from BS.1770, re-derived per rate).
  {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(detail::kPi * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    shelf[0] = static_cast<float>((Vh + Vb * K / Q + K * K) / a0);
    shelf[1] = static_cast<float>(2.0 * (K * K - Vh) / a0);
    shelf[2] = static_cast<float>((Vh - Vb * K / Q + K * K) / a0);
    shelf[3] = static_cast<float>(2.0 * (K * K - 1.0) / a0);
    shelf[4] = static_cast<float>((1.0 - K / Q + K * K) / a0);
  }
  // Stage 2: RLB high-pass.
  {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan(detail::kPi * f0 / fs);
    const double a0 = 1.0 + K / Q + K * K;
    hp[0] = 1.0f;
    hp[1] = -2.0f;
    hp[2] = 1.0f;
    hp[3] = static_cast<float>(2.0 * (K * K - 1.0) / a0);
    hp[4] = static_cast<float>((1.0 - K / Q + K * K) / a0);
  }
}

}  // namespace

void LoudnessMeterReset(LoudnessMeterState& s) noexcept {
  std::memset(s.z, 0, sizeof(s.z));
  s.stepFill = 0;
  s.stepEnergy = 0.0;
  std::memset(s.steps, 0, sizeof(s.steps));
  s.stepWrite = 0;
  s.stepCount = 0;
  s.momentaryEnergy = 0.0;
  s.shortTermEnergy = 0.0;
  s.gatedBlocks = 0;
  std::memset(s.histCount, 0, sizeof(s.histCount));
  std::memset(s.histEnergy, 0, sizeof(s.histEnergy));
}

void LoudnessMeterInit(LoudnessMeterState& s, std::uint32_t channels,
                       std::uint32_t sampleRate) noexcept {
  std::memset(&s, 0, sizeof(s));
  s.channels = channels > kMaxChannels ? kMaxChannels : (channels == 0 ? 1 : channels);
  s.sampleRate = sampleRate > 0 ? sampleRate : 48000;
  s.stepFrames = s.sampleRate / 10;
  KWeighting(static_cast<double>(s.sampleRate), s.shelf, s.highpass);
  for (std::uint32_t c = 0; c < kMaxChannels; ++c) s.weight[c] = 1.0f;
  SpeakerLayout layout;
  if (LayoutForChannels(s.channels, &layout)) {
    const int lfe = LayoutLfeChannel(layout);
    if (lfe >= 0) s.weight[lfe] = 0.0f;
    if (layout == SpeakerLayout::Quad) {
      s.weight[2] = s.weight[3] = 1.41f;
    } else if (layout == SpeakerLayout::Surround51) {
      s.weight[4] = s.weight[5] = 1.41f;
    } else if (layout == SpeakerLayout::Surround71) {
      for (std::uint32_t c = 4; c < 8; ++c) s.weight[c] = 1.41f;
    }
  }
  LoudnessMeterReset(s);
}

namespace {

void CompleteStep(LoudnessMeterState& s) noexcept {
  const double mean = s.stepEnergy / static_cast<double>(s.stepFrames);
  s.steps[s.stepWrite] = mean;
  s.stepWrite = (s.stepWrite + 1u) % 30u;
  if (s.stepCount < 0xFFFFFFFFu) ++s.stepCount;
  s.stepEnergy = 0.0;
  s.stepFill = 0;
  // Momentary (last 4 steps) and short-term (last 30) means.
  const std::uint32_t have = s.stepCount < 30u ? s.stepCount : 30u;
  double sum4 = 0.0, sum30 = 0.0;
  for (std::uint32_t k = 0; k < have; ++k) {
    const double e = s.steps[(s.stepWrite + 30u - 1u - k) % 30u];
    if (k < 4) sum4 += e;
    sum30 += e;
  }
  const std::uint32_t n4 = have < 4u ? have : 4u;
  s.momentaryEnergy = sum4 / static_cast<double>(n4);
  s.shortTermEnergy = sum30 / static_cast<double>(have);
  // A full 400 ms gating block ends every 100 ms (75 % overlap).
  if (s.stepCount >= 4u) {
    const double l = EnergyToLufs(s.momentaryEnergy);
    if (l > kAbsGate) {
      int bin = static_cast<int>((l - kAbsGate) / kBinWidth);
      if (bin < 0) bin = 0;
      if (bin >= static_cast<int>(kLoudnessHistogramBins)) bin = kLoudnessHistogramBins - 1;
      s.histCount[bin] += 1u;
      s.histEnergy[bin] += s.momentaryEnergy;
      ++s.gatedBlocks;
    }
  }
}

}  // namespace

void LoudnessMeterProcess(LoudnessMeterState& s, const float* const* in,
                          std::uint32_t frames) noexcept {
  const std::uint32_t chs = s.channels;
  std::uint32_t pos = 0;
  while (pos < frames) {
    const std::uint32_t n =
        (frames - pos) < (s.stepFrames - s.stepFill) ? (frames - pos) : (s.stepFrames - s.stepFill);
    double energy = 0.0;
    for (std::uint32_t c = 0; c < chs; ++c) {
      const float w = s.weight[c];
      float* z = s.z[c];
      const float* sh = s.shelf;
      const float* hp = s.highpass;
      const float* x = in[c] + pos;
      double acc = 0.0;
      float z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3];
      for (std::uint32_t i = 0; i < n; ++i) {
        const float xi = x[i];
        const float y1 = sh[0] * xi + z0;
        z0 = detail::FlushState(sh[1] * xi - sh[3] * y1 + z1);
        z1 = detail::FlushState(sh[2] * xi - sh[4] * y1);
        const float y2 = hp[0] * y1 + z2;
        z2 = detail::FlushState(hp[1] * y1 - hp[3] * y2 + z3);
        z3 = detail::FlushState(hp[2] * y1 - hp[4] * y2);
        acc += static_cast<double>(y2) * static_cast<double>(y2);
      }
      z[0] = detail::Flush(z0);
      z[1] = detail::Flush(z1);
      z[2] = detail::Flush(z2);
      z[3] = detail::Flush(z3);
      energy += static_cast<double>(w) * acc;
    }
    s.stepEnergy += energy;
    s.stepFill += n;
    pos += n;
    if (s.stepFill >= s.stepFrames) CompleteStep(s);
  }
}

float LoudnessMomentary(const LoudnessMeterState& s) noexcept {
  return static_cast<float>(EnergyToLufs(s.momentaryEnergy));
}

float LoudnessShortTerm(const LoudnessMeterState& s) noexcept {
  return static_cast<float>(EnergyToLufs(s.shortTermEnergy));
}

float LoudnessIntegrated(const LoudnessMeterState& s) noexcept {
  if (s.gatedBlocks == 0) return kLoudnessSilence;
  double e = 0.0;
  std::uint64_t n = 0;
  for (std::uint32_t b = 0; b < kLoudnessHistogramBins; ++b) {
    e += s.histEnergy[b];
    n += s.histCount[b];
  }
  const double rel = EnergyToLufs(e / static_cast<double>(n)) - 10.0;
  double e2 = 0.0;
  std::uint64_t n2 = 0;
  for (std::uint32_t b = 0; b < kLoudnessHistogramBins; ++b) {
    const double centre = kAbsGate + (static_cast<double>(b) + 0.5) * kBinWidth;
    if (centre <= rel) continue;
    e2 += s.histEnergy[b];
    n2 += s.histCount[b];
  }
  if (n2 == 0) return kLoudnessSilence;
  return static_cast<float>(EnergyToLufs(e2 / static_cast<double>(n2)));
}

// ---------------------------------------------------------------------------
// FFT and spectrum
// ---------------------------------------------------------------------------
void FftTwiddles(float* twiddles, std::uint32_t n) noexcept {
  for (std::uint32_t k = 0; k < n / 2; ++k) {
    const double a = -2.0 * detail::kPi * static_cast<double>(k) / static_cast<double>(n);
    twiddles[2 * k] = static_cast<float>(std::cos(a));
    twiddles[2 * k + 1] = static_cast<float>(std::sin(a));
  }
}

void FftComplex(float* data, const float* twiddles, std::uint32_t n) noexcept {
  // Bit-reversal permutation.
  for (std::uint32_t i = 1, j = 0; i < n; ++i) {
    std::uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t0 = data[2 * i], t1 = data[2 * i + 1];
      data[2 * i] = data[2 * j];
      data[2 * i + 1] = data[2 * j + 1];
      data[2 * j] = t0;
      data[2 * j + 1] = t1;
    }
  }
  for (std::uint32_t len = 2; len <= n; len <<= 1) {
    const std::uint32_t half = len >> 1;
    const std::uint32_t step = n / len;
    for (std::uint32_t i = 0; i < n; i += len) {
      for (std::uint32_t k = 0; k < half; ++k) {
        const float wr = twiddles[2 * k * step], wi = twiddles[2 * k * step + 1];
        float* a = data + 2 * (i + k);
        float* b = data + 2 * (i + k + half);
        const float br = b[0] * wr - b[1] * wi;
        const float bi = b[0] * wi + b[1] * wr;
        b[0] = a[0] - br;
        b[1] = a[1] - bi;
        a[0] += br;
        a[1] += bi;
      }
    }
  }
}

namespace {
struct SpectrumHeader {
  std::uint32_t n;
  std::uint32_t write;
  std::uint32_t ringOff, twOff, winOff, workOff;
  float windowSum;
  std::uint32_t reserved[9];
};
static_assert(sizeof(SpectrumHeader) == 64);

bool ValidFftSize(std::uint32_t n) noexcept {
  return n >= 32 && n <= 8192 && (n & (n - 1u)) == 0;
}

std::size_t SpectrumLayout(SpectrumHeader* h, void* base, std::uint32_t n) noexcept {
  detail::Carver c(base, sizeof(SpectrumHeader));
  const std::uint32_t ring = c.Take<float>(n);
  const std::uint32_t tw = c.Take<float>(n);
  const std::uint32_t win = c.Take<float>(n);
  const std::uint32_t work = c.Take<float>(2 * n);
  if (h != nullptr) {
    h->ringOff = ring;
    h->twOff = tw;
    h->winOff = win;
    h->workOff = work;
  }
  return c.Size();
}
}  // namespace

std::size_t SpectrumStateBytes(std::uint32_t fftSize) noexcept {
  if (!ValidFftSize(fftSize)) return 0;
  return SpectrumLayout(nullptr, nullptr, fftSize);
}

bool SpectrumInit(void* state, std::uint32_t fftSize) noexcept {
  if (!ValidFftSize(fftSize) || state == nullptr) return false;
  auto* h = static_cast<SpectrumHeader*>(state);
  std::memset(h, 0, sizeof(*h));
  h->n = fftSize;
  SpectrumLayout(h, state, fftSize);
  FftTwiddles(detail::At<float>(state, h->twOff), fftSize);
  float* w = detail::At<float>(state, h->winOff);
  double sum = 0.0;
  for (std::uint32_t i = 0; i < fftSize; ++i) {
    w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * detail::kPi * i / fftSize));
    sum += w[i];
  }
  h->windowSum = static_cast<float>(sum);
  return true;
}

std::uint32_t SpectrumSize(const void* state) noexcept {
  return static_cast<const SpectrumHeader*>(state)->n;
}

void SpectrumPush(void* state, const float* const* in, std::uint32_t channels,
                  std::uint32_t frames) noexcept {
  auto* h = static_cast<SpectrumHeader*>(state);
  float* ring = detail::At<float>(state, h->ringOff);
  if (channels == 0) return;
  const float inv = 1.0f / static_cast<float>(channels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    float s = 0.0f;
    for (std::uint32_t c = 0; c < channels; ++c) s += in[c][i];
    ring[h->write] = s * inv;
    h->write = (h->write + 1u) & (h->n - 1u);
  }
}

void SpectrumCompute(void* state, float* magnitudes) noexcept {
  auto* h = static_cast<SpectrumHeader*>(state);
  const std::uint32_t n = h->n;
  const float* ring = detail::At<float>(state, h->ringOff);
  const float* w = detail::At<float>(state, h->winOff);
  float* work = detail::At<float>(state, h->workOff);
  for (std::uint32_t i = 0; i < n; ++i) {
    work[2 * i] = ring[(h->write + i) & (n - 1u)] * w[i];  // oldest first
    work[2 * i + 1] = 0.0f;
  }
  FftComplex(work, detail::At<float>(state, h->twOff), n);
  const float scale = 2.0f / h->windowSum;
  for (std::uint32_t k = 0; k <= n / 2; ++k) {
    const float re = work[2 * k], im = work[2 * k + 1];
    const float m = std::sqrt(re * re + im * im) * scale;
    magnitudes[k] = (k == 0 || k == n / 2) ? m * 0.5f : m;
  }
}

}  // namespace snd::dsp
