// Drives one of the stage 7-D kinds (Oscillator, Noise, Input) directly, without
// core: a source::Runtime, the kind's pool, and RenderBatch with items the way core
// makes them (zeroed buffers, output-rate items get the pitch ramp, source-rate
// items frameOffset 0). Every call into the kind after Init runs inside an RT scope
// (rt_check.hpp: allocation, locks, IO fail the test); renderUs sums the time spent
// in RenderBatch. Plus small analysis helpers (FFT, Goertzel, band power).
#ifndef SND_SOURCE_TEST_SYNTH_DRIVER_HPP
#define SND_SOURCE_TEST_SYNTH_DRIVER_HPP

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/source/runtime.hpp"

namespace synthtest {

namespace src = snd::source;

// Per-block modulation (output-rate items).
struct BlockMod {
  snd::Ramp pitch{1.0f, 1.0f};
  snd::Ramp duty{0.0f, 0.0f};
  snd::Ramp table{0.0f, 0.0f};
};

class Driver {
 public:
  struct Opt {
    std::uint32_t maxVoices = 16;
    std::uint32_t blockFrames = 256;
    std::uint32_t sampleRate = 48000;
    std::uint32_t inputPorts = 2;
    std::uint32_t inputPortFrames = 4096;
    std::uint32_t inputPortChannels = 2;
    snd::IAllocator* allocator = nullptr;
  };
  struct Voice {
    snd::SlotIndex slot = snd::kInvalidSlot;
    snd::Playhead ph{};
    std::uint32_t channels = 1;
    std::vector<std::vector<float>> out;  // per channel, appended
    snd::VoiceRenderStatus last{};
    std::uint32_t starvedBlocks = 0;
    std::uint32_t reserved = 0;  // SourceStartResult::reserved
    std::uint32_t endDetail = 0;
  };
  using Mod = BlockMod;

  Driver(snd::SourceKind kind, const Opt& o) : opt_(o) {
    alloc_ = o.allocator != nullptr ? o.allocator : &snd::base::DefaultAllocator();
    src::RuntimeDesc rd;
    rd.allocator = alloc_;
    rd.inputPorts = o.inputPorts;
    rd.inputPortFrames = o.inputPortFrames;
    rd.inputPortChannels = o.inputPortChannels;
    ok_ = runtime.Init(rd);
    lim_.maxVoices = o.maxVoices;
    lim_.maxRealVoices = o.maxVoices;
    lim_.blockFrames = o.blockFrames;
    lim_.sampleRate = o.sampleRate;
    lim_.callbackAheadBlocks = 4;
    sv_.allocator = alloc_;
    kind_ = ok_ ? runtime.Kinds().kinds[static_cast<std::size_t>(kind)] : nullptr;
    if (kind_ != nullptr) {
      const std::size_t bytes = kind_->PoolBytes(lim_);
      pool_ = static_cast<std::uint8_t*>(alloc_->Allocate(bytes, 64, snd::MemTag::Misc));
      ok_ = pool_ != nullptr && kind_->Init(pool_, bytes, lim_, sv_);
    } else {
      ok_ = false;
    }
  }
  ~Driver() {
    if (kind_ != nullptr && ok_) kind_->Shutdown();
    if (pool_ != nullptr) alloc_->Free(pool_, snd::MemTag::Misc);
    runtime.Destroy();
  }
  Driver(const Driver&) = delete;
  Driver& operator=(const Driver&) = delete;

  bool Ok() const { return ok_; }
  src::ISourceKind& Kind() { return *kind_; }

  // Starts a voice; returns false when the kind refused it.
  bool Start(Voice& v, const void* params, std::uint32_t channels, std::uint32_t seed = 0, std::uint64_t startFrame = 0) {
    src::SourceStartRequest rq{};
    rq.voiceSlot = 0;
    rq.params = params;
    rq.channels = channels;
    rq.seed = seed;
    rq.startFrame = startFrame;
    src::SourceStartResult res{};
    {
      snd::rt::RtScope rt;
      kind_->Start(&rq, 1, &res);
    }
    v.slot = res.sourceSlot;
    v.channels = channels;
    v.reserved = res.reserved;
    v.endDetail = res.endDetail;
    v.out.assign(channels, {});
    return res.sourceSlot != snd::kInvalidSlot;
  }
  void Release(Voice& v) {
    {
      snd::rt::RtScope rt;
      kind_->Release(&v.slot, 1);
    }
    v.slot = snd::kInvalidSlot;
  }
  void Control(Voice& v, src::ControlOp op, std::uint64_t frame = 0) {
    src::SourceControlOp c{};
    c.sourceSlot = v.slot;
    c.op = op;
    c.frame = frame;
    snd::rt::RtScope rt;
    kind_->Control(&c, 1);
  }
  // One block for every voice in `vs` (one batch). frames = output frames (output-rate
  // kinds: from frameOffset to the block's end) or source frames (source-rate kinds).
  void Block(std::vector<Voice*> vs, const Mod& m = {}, std::uint32_t frames = 0, std::uint32_t frameOffset = 0,
             std::uint32_t blockFrames = 0) {
    const std::uint32_t bf = blockFrames != 0u ? blockFrames : opt_.blockFrames;
    const std::uint32_t n = frames != 0u ? frames : bf - frameOffset;
    const bool outRate = kind_->RendersAtOutputRate();
    const std::uint32_t total = outRate ? frameOffset + n : n;
    std::vector<src::RenderItem> items(vs.size());
    std::vector<std::vector<float>> bufs;
    std::vector<std::vector<float*>> ptrs(vs.size());
    for (std::size_t i = 0; i < vs.size(); ++i) {
      src::RenderItem it{};
      it.voiceSlot = static_cast<snd::SlotIndex>(i);
      it.sourceSlot = vs[i]->slot;
      it.realSlot = static_cast<snd::SlotIndex>(i);
      it.frameOffset = outRate ? frameOffset : 0u;
      it.framesRequested = n;
      it.channels = vs[i]->channels;
      it.pitch = m.pitch;
      it.dutyOffset = m.duty;
      it.tablePositionOffset = m.table;
      it.playhead = &vs[i]->ph;
      it.transportBeats = -1.0;
      for (std::uint32_t c = 0; c < vs[i]->channels; ++c) {
        bufs.emplace_back(total + 16u, 0.0f);
        ptrs[i].push_back(bufs.back().data());
      }
      it.out = ptrs[i].data();
      items[i] = it;
    }
    std::vector<snd::VoiceRenderStatus> st(vs.size());
    src::RenderBatch b{};
    b.kind = kind_->Kind();
    b.count = static_cast<std::uint32_t>(vs.size());
    b.items = items.data();
    b.block.frames = bf;
    b.block.sampleRate = opt_.sampleRate;
    b.block.clock = clock_;
    b.scratch = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    {
      snd::rt::RtScope rt;
      kind_->RenderBatch(b, st.data());
    }
    renderUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    clock_ += bf;
    for (std::size_t i = 0; i < vs.size(); ++i) {
      vs[i]->last = st[i];
      if (st[i].starved != 0u) ++vs[i]->starvedBlocks;
      const std::uint32_t from = outRate ? frameOffset : 0u;
      for (std::uint32_t c = 0; c < vs[i]->channels; ++c) {
        const float* p = ptrs[i][c];
        vs[i]->out[c].insert(vs[i]->out[c].end(), p + from, p + from + n);
      }
    }
  }
  // Renders `frames` output frames for one voice in blocks of `block` (the last partial).
  void Render(Voice& v, std::size_t frames, const Mod& m = {}, std::uint32_t block = 0) {
    const std::uint32_t bf = block != 0u ? block : opt_.blockFrames;
    std::size_t done = 0;
    while (done < frames) {
      const std::uint32_t n = static_cast<std::uint32_t>(frames - done < bf ? frames - done : bf);
      Block({&v}, m, n, 0, bf);
      done += n;
    }
  }
  void Advance(Voice& v, std::uint32_t frames, float pitch = 1.0f) {
    src::AdvanceItem a{0, v.slot, frames, pitch, &v.ph};
    snd::VoiceRenderStatus st{};
    snd::rt::RtScope rt;
    kind_->AdvanceVirtual(&a, 1, &st);
    v.last = st;
  }

  src::Runtime runtime;
  double renderUs = 0.0;

 private:
  Opt opt_;
  snd::IAllocator* alloc_ = nullptr;
  src::SourceLimits lim_{};
  src::SourceServices sv_{};
  src::ISourceKind* kind_ = nullptr;
  std::uint8_t* pool_ = nullptr;
  snd::ClockSample clock_ = 0;
  bool ok_ = false;
};

// --- analysis ------------------------------------------------------------------------
// In-place radix-2 FFT (n a power of two).
inline void Fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0u; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -6.283185307179586 / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Power spectrum (|X|^2, bins 0..n/2) of x[from, from + n) with a Blackman-Harris window.
inline std::vector<double> Power(const std::vector<float>& x, std::size_t from, std::size_t n) {
  std::vector<std::complex<double>> a(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = 6.283185307179586 * static_cast<double>(i) / static_cast<double>(n);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    a[i] = std::complex<double>(static_cast<double>(x[from + i]) * w, 0.0);
  }
  Fft(a);
  std::vector<double> p(n / 2 + 1);
  for (std::size_t k = 0; k <= n / 2; ++k) p[k] = std::norm(a[k]);
  return p;
}

// Amplitude of `hz` in x[from, from + n) (Hann window, coherent gain corrected).
inline double Tone(const std::vector<float>& x, std::size_t from, std::size_t n, double hz, double rate = 48000.0) {
  const double w = 6.283185307179586 * hz / rate;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = x[from + i] * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}

// Share of the power (dB, relative to the total) outside +-guard bins of the harmonics
// k f0 below Nyquist: what aliasing folded back. Bin 0..2 (DC) excluded.
inline double AliasDb(const std::vector<double>& p, double f0, double rate, std::size_t n, std::size_t guard = 6) {
  std::vector<bool> harm(p.size(), false);
  for (double f = f0; f < rate / 2; f += f0) {
    const std::size_t c = static_cast<std::size_t>(std::llround(f * static_cast<double>(n) / rate));
    for (std::size_t k = c > guard ? c - guard : 0; k <= c + guard && k < p.size(); ++k) harm[k] = true;
  }
  double alias = 0, total = 0;
  for (std::size_t k = 3; k < p.size(); ++k) {
    total += p[k];
    if (!harm[k]) alias += p[k];
  }
  return 10.0 * std::log10(alias / total + 1e-30);
}

inline double Rms(const std::vector<float>& x, std::size_t from = 0, std::size_t n = 0) {
  if (n == 0) n = x.size() - from;
  double s = 0;
  for (std::size_t i = 0; i < n; ++i) s += double(x[from + i]) * x[from + i];
  return std::sqrt(s / double(n));
}

}  // namespace synthtest

#endif  // SND_SOURCE_TEST_SYNTH_DRIVER_HPP
