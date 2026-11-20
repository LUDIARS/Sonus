// Analysis (stage 3 group 4, D-147 / D-148): see analysis.hpp.
#include "analysis.hpp"

#include <cmath>
#include <cstring>
#include <limits>

#include "snd/base/atomic.hpp"

namespace snd::core {

namespace {

template <class T>
T* Take(Impl& im, std::size_t count, MemTag tag) noexcept {
  if (count == 0u) count = 1u;
  if (im.allocationCount >= im.allocationCap) return nullptr;
  T* p = base::AllocateArray<T>(*im.alloc, count, tag);
  if (p == nullptr) return nullptr;
  im.allocations[im.allocationCount++] = Impl::Allocation{p, tag, count * sizeof(T)};
  return p;
}

MeterHeader* HeaderOf(const AnalysisShared& a, std::uint8_t* data, std::uint32_t m) noexcept {
  return reinterpret_cast<MeterHeader*>(data + a.meterBytes * m);
}
float* SamplesOf(const AnalysisShared& a, std::uint8_t* data, std::uint32_t m) noexcept {
  return reinterpret_cast<float*>(data + a.meterBytes * m + sizeof(MeterHeader));
}

void ResetMeter(Impl& im, MeterSlot& ms, SlotIndex bus, std::uint32_t gen, Id id, std::uint32_t channels) noexcept {
  const AnalysisShared& a = *im.rd.analysis;
  ms.bus = bus;
  ms.busGen = gen;
  ms.busId = id;
  ms.channels = channels > a.ringChannels ? a.ringChannels : channels;
  ms.write = 0;
  ms.filled = 0;
  std::memset(ms.ring, 0, sizeof(float) * std::size_t{a.ringChannels} * a.fftSize);
  // Loudness of all the bus's channels (the BS.1770 channel weights need the layout).
  dsp::LoudnessMeterInit(*ms.loudness, channels == 0u ? 1u : channels, im.cfg.sampleRate);
}

}  // namespace

bool InitAnalysis(Impl& im) noexcept {
  RenderPriv& rd = im.rd;
  const CoreLimits& lim = im.desc.limits;
  std::uint32_t fft = lim.spectrumFftSize;
  if (fft < 32u || fft > 8192u || (fft & (fft - 1u)) != 0u) fft = 1024u;
  rd.meterCount = lim.maxMeteredBuses + 1u;
  rd.meters = Take<MeterSlot>(im, rd.meterCount, MemTag::Tables);
  rd.busMeter = Take<std::uint32_t>(im, im.cfg.busCap, MemTag::Tables);
  rd.analysis = Take<AnalysisShared>(im, 1u, MemTag::Notify);
  if (rd.meters == nullptr || rd.busMeter == nullptr || rd.analysis == nullptr) return false;
  AnalysisShared& a = *rd.analysis;
  a.fftSize = fft;
  a.ringChannels = im.cfg.outChannels < 2u ? 2u : im.cfg.outChannels;
  a.meterCount = rd.meterCount;
  a.meterBytes = sizeof(MeterHeader) + sizeof(float) * std::size_t{a.ringChannels} * fft;
  for (auto& f : a.frames) {
    f.data = Take<std::uint8_t>(im, a.meterBytes * a.meterCount, MemTag::Notify);
    if (f.data == nullptr) return false;
  }
  a.spectrum = Take<std::uint8_t>(im, dsp::SpectrumStateBytes(fft), MemTag::Notify);
  a.samples = Take<float>(im, fft, MemTag::Notify);
  a.raw = Take<float>(im, fft, MemTag::Notify);
  a.magnitudes = Take<float>(im, fft / 2u + 1u, MemTag::Notify);
  if (a.spectrum == nullptr || a.samples == nullptr || a.raw == nullptr || a.magnitudes == nullptr || !dsp::SpectrumInit(a.spectrum, fft)) {
    return false;
  }
  for (std::uint32_t s = 0; s < im.cfg.busCap; ++s) rd.busMeter[s] = kNoSlot;
  for (std::uint32_t m = 0; m < rd.meterCount; ++m) {
    MeterSlot& ms = rd.meters[m];
    ms.loudness = Take<dsp::LoudnessMeterState>(im, 1u, MemTag::Tables);
    ms.ring = Take<float>(im, std::size_t{a.ringChannels} * fft, MemTag::Tables);
    if (ms.loudness == nullptr || ms.ring == nullptr) return false;
    ms.bus = kNoSlot;
  }
  // Meter 0: the output (always measured).
  ResetMeter(im, rd.meters[0], kNoSlot, 0u, 0u, im.cfg.outChannels);
  return true;
}

// Render (MixBuses, before the buses): frees the meters of buses that are gone.
void AnalysisMaintain(Impl& im) noexcept {
  RenderPriv& rd = im.rd;
  const schema::TableRows& br = im.tables.Bus.rows;
  for (std::uint32_t m = 1; m < rd.meterCount; ++m) {
    MeterSlot& ms = rd.meters[m];
    if (ms.bus == kNoSlot) continue;
    if (!base::rows::IsLive(br, ms.bus) || br.generation[ms.bus] != ms.busGen) {
      if (ms.bus < im.cfg.busCap && rd.busMeter[ms.bus] == m) rd.busMeter[ms.bus] = kNoSlot;
      ms.bus = kNoSlot;
    }
  }
}

// Render (MixBuses): after the fader, with the peak / RMS meters of the bus.
void AnalysisMixBus(Impl& im, SlotIndex bus, std::uint32_t d, const float* const* chans, std::uint32_t channels) noexcept {
  (void)d;
  RenderPriv& rd = im.rd;
  const schema::TableRows& br = im.tables.Bus.rows;
  if (bus >= im.cfg.busCap || bus == rd.masterSlot) return;  // the master is meter 0 (the output)
  std::uint32_t m = rd.busMeter[bus];
  if (m != kNoSlot && (rd.meters[m].bus != bus || rd.meters[m].busGen != br.generation[bus])) m = kNoSlot;
  if (m == kNoSlot) {
    if ((im.busDefs[bus].flags & bf::kBusMeter) == 0u) return;
    for (std::uint32_t k = 1; k < rd.meterCount && m == kNoSlot; ++k) {
      if (rd.meters[k].bus == kNoSlot) m = k;
    }
    if (m == kNoSlot) return;  // every meter is taken: this bus has none
    rd.busMeter[bus] = m;
    ResetMeter(im, rd.meters[m], bus, br.generation[bus], im.busDefs[bus].id, channels);
  }
  MeterSlot& ms = rd.meters[m];
  dsp::LoudnessMeterProcess(*ms.loudness, chans, im.cfg.blockFrames);
  const AnalysisShared& a = *rd.analysis;
  const std::uint32_t frames = im.cfg.blockFrames;
  for (std::uint32_t c = 0; c < ms.channels; ++c) {
    float* ring = ms.ring + std::size_t{c} * a.fftSize;
    std::uint32_t w = ms.write;
    for (std::uint32_t f = 0; f < frames; ++f) {
      ring[w] = chans[c][f];
      w = (w + 1u) & (a.fftSize - 1u);
    }
  }
  ms.write = (ms.write + frames) & (a.fftSize - 1u);
  ms.filled = ms.filled + frames > a.fftSize ? a.fftSize : ms.filled + frames;
}

// Render (MixBuses, after the buses): meter 0 measures the output.
void AnalysisMixOutput(Impl& im) noexcept {
  RenderPriv& rd = im.rd;
  MeterSlot& ms = rd.meters[0];
  const AnalysisShared& a = *rd.analysis;
  const std::uint32_t frames = im.cfg.blockFrames;
  const float* chans[kMaxChannels];
  for (std::uint32_t c = 0; c < im.cfg.outChannels; ++c) chans[c] = rd.outBuf + std::size_t{c} * frames;
  dsp::LoudnessMeterProcess(*ms.loudness, chans, frames);
  for (std::uint32_t c = 0; c < ms.channels; ++c) {
    float* ring = ms.ring + std::size_t{c} * a.fftSize;
    std::uint32_t w = ms.write;
    for (std::uint32_t f = 0; f < frames; ++f) {
      ring[w] = chans[c][f];
      w = (w + 1u) & (a.fftSize - 1u);
    }
  }
  ms.write = (ms.write + frames) & (a.fftSize - 1u);
  ms.filled = ms.filled + frames > a.fftSize ? a.fftSize : ms.filled + frames;
}

// Publish: loudness of a bus (meter 0 for the master / the output). false = not measured.
bool AnalysisLoudness(const Impl& im, SlotIndex bus, float* shortTerm, float* integrated) noexcept {
  const RenderPriv& rd = im.rd;
  std::uint32_t m = kNoSlot;
  if (bus == kNoSlot || bus == rd.masterSlot) {
    m = 0u;
  } else if (bus < im.cfg.busCap) {
    m = rd.busMeter[bus];
    if (m != kNoSlot && rd.meters[m].bus != bus) m = kNoSlot;
  }
  if (m == kNoSlot) {
    *shortTerm = std::numeric_limits<float>::quiet_NaN();
    *integrated = std::numeric_limits<float>::quiet_NaN();
    return false;
  }
  *shortTerm = dsp::LoudnessShortTerm(*rd.meters[m].loudness);
  *integrated = dsp::LoudnessIntegrated(*rd.meters[m].loudness);
  return true;
}

// Publish: the rings into the back frame while a reader is interested (D-148).
void AnalysisPublish(Impl& im) noexcept {
  RenderPriv& rd = im.rd;
  AnalysisShared& a = *rd.analysis;
  if (a.wanted.exchange(0u, std::memory_order_acq_rel) != 0u) a.interestLeft = kSpectrumInterestBlocks;
  if (a.interestLeft == 0u) return;
  --a.interestLeft;
  AnalysisFrame& f = a.frames[a.back];
  const std::uint32_t s = f.seq.load(std::memory_order_relaxed);
  f.seq.store(s + 1u, std::memory_order_relaxed);
  for (std::uint32_t m = 0; m < rd.meterCount; ++m) {
    const MeterSlot& ms = rd.meters[m];
    MeterHeader h{ms.busId, ms.bus == kNoSlot && m != 0u ? 0u : ms.channels, ms.write, ms.filled};
    base::AtomicCopyIn(HeaderOf(a, f.data, m), &h, sizeof(h));
    if (h.channels == 0u) continue;
    base::AtomicCopyIn(SamplesOf(a, f.data, m), ms.ring, sizeof(float) * std::size_t{ms.channels} * a.fftSize);
  }
  f.seq.store(s + 2u, std::memory_order_release);
  const std::uint32_t prev = a.middle.exchange(a.back | kAnalysisDirty, std::memory_order_acq_rel);
  a.back = prev & 3u;
}

// Update (owner thread): the newest published frame becomes the front.
void AnalysisSwap(Impl& im) noexcept {
  AnalysisShared& a = *im.rd.analysis;
  if ((a.middle.load(std::memory_order_relaxed) & kAnalysisDirty) == 0u) return;
  const std::uint32_t prev = a.middle.exchange(a.front, std::memory_order_acq_rel);
  a.front = prev & 3u;
  AnalysisFrame& f = a.frames[a.front];
  f.frontSeq.store(f.seq.load(std::memory_order_acquire), std::memory_order_release);
  a.frontPublished.store(a.front, std::memory_order_release);
}

// Any thread (D-148). Returns SND_OK with min(binCount, fftSize / 2 + 1) magnitudes (the
// rest of `out` is 0): bin k is k * sampleRate / fftSize Hz, linear, a full-scale sine on
// a bin reads 1 (dsp SpectrumCompute: Hann window). SND_ERR_INVALID_ARGUMENT: null out,
// binCount 0. SND_ERR_NOT_FOUND: no such bus (or not a loaded one). SND_ERR_UNSUPPORTED:
// the bus has no meter (no kBusMeter, or all meters taken) or channel >= its measured
// channels. SND_ERR_NOT_READY: no frame published yet (nothing read for a while: the
// interest is renewed by this call; ready after the next Update), or the frame changed
// under 1024 attempts.
snd_result System::GetSpectrum(Id bus, std::uint32_t channel, float* out, std::uint32_t binCount) const noexcept {
  Impl& im = *impl_;
  if (out == nullptr || binCount == 0u) return SND_ERR_INVALID_ARGUMENT;
  AnalysisShared& a = *im.rd.analysis;
  a.wanted.store(1u, std::memory_order_release);  // keep (or start) publishing the rings
  // The bus must exist in the latest snapshot; the master (index 0) is meter 0.
  snd_meter meter{};
  const snd_result mr = GetMeter(bus, &meter);
  if (mr != SND_OK) return mr;
  Id masterId = 0;
  base::notify::ReadFront(*im.triple, [&](const notify::SnapshotFrame& f) {
    const base::notify::FrameHeader hdr = base::notify::GetFrameHeader(f);
    masterId = hdr.busCount != 0u ? base::notify::SnapshotLoad(&f.buses[0]).busId : 0u;
  });
  const bool master = bus == 0u || bus == masterId;
  base::LockGuard lock(a.readerMutex);
  const std::uint32_t n = a.fftSize;
  for (std::uint32_t attempt = 0; attempt < 1024u; ++attempt) {
    const std::uint32_t idx = a.frontPublished.load(std::memory_order_acquire) & 3u;
    AnalysisFrame& f = a.frames[idx];
    const std::uint32_t s1 = f.seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u || s1 != f.frontSeq.load(std::memory_order_acquire)) continue;
    if (s1 == 0u) return SND_ERR_NOT_READY;  // nothing published since a reader asked
    std::uint32_t m = master ? 0u : kNoSlot;
    MeterHeader h{};
    for (std::uint32_t k = 1; k < a.meterCount && m == kNoSlot; ++k) {
      base::AtomicCopyOut(&h, HeaderOf(a, f.data, k), sizeof(h));
      if (h.channels != 0u && h.busId == bus) m = k;
    }
    if (m != kNoSlot) base::AtomicCopyOut(&h, HeaderOf(a, f.data, m), sizeof(h));
    const bool ok = m != kNoSlot && channel < h.channels;
    if (ok) base::AtomicCopyOut(a.raw, SamplesOf(a, f.data, m) + std::size_t{channel} * n, sizeof(float) * n);
    if (f.seq.load(std::memory_order_acquire) != s1 || (a.frontPublished.load(std::memory_order_acquire) & 3u) != idx) {
      continue;  // rewritten while copying: again
    }
    if (!ok) return SND_ERR_UNSUPPORTED;  // no meter on this bus, or no such channel
    for (std::uint32_t i = 0; i < n; ++i) a.samples[i] = a.raw[(h.write + i) & (n - 1u)];  // oldest first
    const float* in[1] = {a.samples};
    dsp::SpectrumPush(a.spectrum, in, 1u, n);  // exactly n frames: the window holds them in order
    dsp::SpectrumCompute(a.spectrum, a.magnitudes);
    const std::uint32_t bins = n / 2u + 1u;
    for (std::uint32_t k = 0; k < binCount; ++k) out[k] = k < bins ? a.magnitudes[k] : 0.0f;
    return SND_OK;
  }
  return SND_ERR_NOT_READY;
}

}  // namespace snd::core
