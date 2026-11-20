// Independent tests of stage 3 group 2 (standard DSP through core, runtime
// effect parameters D-130, compressor sidechain D-131, voice effects
// D-132..D-134). Written by the independent tester from the specification
// (design.md 2.4, 2.5, 4, 7; the public headers sonus.h / config.h / types.h /
// effects.hpp; core.hpp) before reading the implementation.
//
// Builds on indep_fixture.hpp (IndepKind: DC / counter / finite DC sources at
// the output rate) and adds:
//  - G2Config(): the indep config with the load LOD kept off (design 2.5: bit
//    identity holds with the LOD disabled) unless a test sets it.
//  - Probe: a plugin effect (Config.plugins) that multiplies by its parameter 0
//    (linear ramp from ParamBlock start to end over the call) and records every
//    call (state id, start/end of parameter 0, channels, frames). Its state
//    remembers the last input sample, which reveals a state leak between voices.
//  - Analysis helpers on the rendered stream.
#ifndef SND_CORE_TEST_INDEP_G2_FIXTURE_HPP
#define SND_CORE_TEST_INDEP_G2_FIXTURE_HPP

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "indep_fixture.hpp"
#include "snd/dsp/effects.hpp"

namespace g2 {

using namespace indep;
namespace dsp = snd::dsp;

inline snd_config G2Config() {
  snd_config c = indep::Config();
  c.tuning.lod_down_blocks = 0x7FFFFFFFu;  // never lower the quality (2.5 condition)
  c.tuning.lod_up_blocks = 1u;
  return c;
}

// Counts every allocation (initialisation vs steady state, 3.3).
class CountingAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    allocations.fetch_add(1, std::memory_order_relaxed);
    return snd::base::DefaultAllocator().Allocate(bytes, align, tag);
  }
  void Free(void* p, snd::MemTag tag) override {
    frees.fetch_add(1, std::memory_order_relaxed);
    snd::base::DefaultAllocator().Free(p, tag);
  }
  std::atomic<std::uint64_t> allocations{0}, frees{0};
};

// --- Probe plugin --------------------------------------------------------------------
inline constexpr snd::Id kProbeId = snd::HashId("test.g2.probe");

struct ProbeCall {
  std::uint32_t stateId;
  std::uint32_t channels;
  std::uint32_t frames;
  float start0;
  float end0;
  float firstIn;  // first input sample of channel 0
  float stateLastIn;  // what the state remembered before this call
};

struct ProbeState {
  std::uint32_t magic;
  std::uint32_t id;
  float gain;      // last end value of parameter 0
  float lastIn;    // last input sample of channel 0 of the previous call
  std::uint32_t calls;
};

class ProbeLog {
 public:
  static constexpr std::uint32_t kCap = 1u << 15;
  void Reset() {
    n.store(0);
    inits.store(0);
    nextId.store(1);
  }
  void Add(const ProbeCall& c) {
    const std::uint32_t i = n.fetch_add(1);
    if (i < kCap) calls[i] = c;
  }
  std::uint32_t Count() const { return n.load() < kCap ? n.load() : kCap; }
  ProbeCall calls[kCap];
  std::atomic<std::uint32_t> n{0}, inits{0}, nextId{1};
};

inline ProbeLog& Log() {
  static ProbeLog* log = new ProbeLog();  // never freed: plugin calls may outlive a test's objects
  return *log;
}

inline std::uint32_t SND_CALL ProbeBytes(void*, const snd_effect_desc*) { return 64u; }
inline void SND_CALL ProbeInit(void*, void* state, const snd_effect_desc* desc, std::uint32_t) {
  auto* s = static_cast<ProbeState*>(state);
  s->magic = 0x50524F42u;
  s->id = Log().nextId.fetch_add(1);
  s->gain = (desc != nullptr && desc->param_count > 0u && desc->initial_params != nullptr) ? desc->initial_params[0] : 1.0f;
  s->lastIn = 0.0f;
  s->calls = 0;
  Log().inits.fetch_add(1);
}
inline void SND_CALL ProbeProcess(void*, void* state, const float* const* in, float* const* out, std::uint32_t channels,
                                  std::uint32_t frames, const snd_param_block* p) {
  auto* s = static_cast<ProbeState*>(state);
  float a = s->gain, b = s->gain;
  if (p != nullptr && p->count > 0u && p->start != nullptr && p->end != nullptr) {
    a = p->start[0];
    b = p->end[0];
  }
  ProbeCall c{s->id, channels, frames, a, b, frames > 0u && channels > 0u ? in[0][0] : 0.0f, s->lastIn};
  Log().Add(c);
  const float lastIn = frames > 0u && channels > 0u ? in[0][frames - 1u] : s->lastIn;
  for (std::uint32_t ch = 0; ch < channels; ++ch) {
    for (std::uint32_t i = 0; i < frames; ++i) {
      const float g = a + (b - a) * (static_cast<float>(i + 1u) / static_cast<float>(frames));
      out[ch][i] = in[ch][i] * g;
    }
  }
  s->gain = b;
  s->lastIn = lastIn;
  ++s->calls;
}
inline int32_t SND_CALL ProbeTail(void*, const void*) { return 1; }

inline snd_dsp_effect ProbePlugin() {
  snd_dsp_effect e{};
  e.user = nullptr;
  e.effect_type = kProbeId;
  e.state_bytes = &ProbeBytes;
  e.init = &ProbeInit;
  e.process = &ProbeProcess;
  e.is_tail_done = &ProbeTail;
  return e;
}

// --- Raw command injection (variant checks) ---------------------------------------------
// A System like indep::Fx whose LiveTap::replay hands ApplyCommands one arena of raw commands
// (header + payload) at the block the test chooses (the arena is applied like a producer's).
class RawFx {
 public:
  explicit RawFx(const snd_config& cfg = G2Config()) : cfg_(cfg) {
    kinds_.kinds[static_cast<int>(snd::SourceKind::Oscillator)] = &kind;
    snd::core::CoreDesc d;
    d.config = &cfg_;
    d.allocator = &snd::base::DefaultAllocator();
    d.sources = &kinds_;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    d.liveTap.user = this;
    d.liveTap.replay = &Replay;
    snd::core::System::Create(d, &sys);
    if (sys != nullptr) {
      snd_callbacks cb{};
      cb.user = this;
      cb.on_error = [](void* u, snd_result r, std::uint32_t det, std::uint64_t o) {
        static_cast<RawFx*>(u)->errors.push_back(Err{r, det, o});
      };
      sys->SetCallbacks(&cb);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~RawFx() { snd::core::System::Destroy(sys); }
  RawFx(const RawFx&) = delete;
  RawFx& operator=(const RawFx&) = delete;
  // Commands queued between two Step() calls are applied in the next block.
  template <class P>
  void Queue(snd::cmd::CommandType type, const P& payload) {
    if (!ready_) pending_.clear();
    ready_ = true;
    const std::uint32_t size = static_cast<std::uint32_t>((sizeof(snd::cmd::CommandHeader) + sizeof(P) + 7u) & ~7u);
    const std::size_t at = pending_.size();
    pending_.resize(at + size, 0);
    snd::cmd::CommandHeader hd{static_cast<std::uint16_t>(type), static_cast<std::uint16_t>(size), 0};
    std::memcpy(pending_.data() + at, &hd, sizeof(hd));
    std::memcpy(pending_.data() + at + sizeof(hd), &payload, sizeof(P));
  }
  void Step() {
    sys->Update();
    tAudioThread = true;
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
  }
  void Steps(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Step();
  }
  snd_bank_handle Load(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    fs.AddFile(path, bytes);
    const snd_bank_handle h = sys->LoadBankAsync(path.c_str());
    for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) Step();
    sys->Update();
    return h;
  }

  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  IndepKind kind;
  snd::core::System* sys = nullptr;
  std::vector<Err> errors;
  std::vector<float> out, all;

 private:
  static bool Replay(void* user, snd::BlockIndex block, snd::core::LiveArenaView* next) noexcept {
    auto* self = static_cast<RawFx*>(user);
    if (!self->ready_ || self->given_) {
      if (self->given_) {  // the arena was applied in this block's loop: done
        self->ready_ = false;
        self->given_ = false;
      }
      return false;
    }
    *next = snd::core::LiveArenaView{};
    next->producer = snd::kInvalidSlot;
    next->appliedBlock = block;
    next->normal = self->pending_.data();
    next->normalBytes = static_cast<std::uint32_t>(self->pending_.size());
    self->given_ = true;
    return true;
  }
  snd_config cfg_;
  snd::source::SourceKindSet kinds_{};
  std::vector<std::uint8_t> pending_;
  bool ready_ = false, given_ = false;
};

// --- Banks ---------------------------------------------------------------------------
// Rows: Sfx (0) -> Master (1); helpers to add effects to a bus.
struct FxBank {
  TestBank b;
  FxBank() {
    b.buses.push_back(TBus{snd::HashId("Sfx"), 1u, 0, 0, 0.0f, {}, {}});
    b.buses.push_back(TBus{snd::HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  }
  std::uint32_t Effect(snd::Id type, std::vector<float> params) {
    b.effects.push_back(TEffect{type, std::move(params)});
    return static_cast<std::uint32_t>(b.effects.size() - 1u);
  }
  std::uint32_t Synth(float f, std::uint32_t channels = 1) {
    b.synths.push_back(TSynth{0, 0, channels, f});
    return static_cast<std::uint32_t>(b.synths.size() - 1u);
  }
  TEvent& Event(const char* name, float f, std::uint32_t bus = 0, std::uint32_t priority = 128,
                std::uint32_t channels = 1) {
    const std::uint32_t s = Synth(f, channels);
    TEvent e;
    e.id = snd::HashId(name);
    e.priority = priority;
    e.bus = bus;
    TTrack t;
    TClip c;
    c.kind = 2;
    c.target = s;
    t.clips.push_back(c);
    e.tracks.push_back(t);
    b.events.push_back(e);
    return b.events.back();
  }
  std::vector<std::uint8_t> Build(const char* id) const { return b.Build(snd::HashId(id)); }
};

// Echo used as a pure gain: Delay 10 ms, Decay 0, DryMix d, WetMix 0, MaxDelay 10 ms.
inline std::vector<float> EchoGain(float dry) { return {10.0f, 0.0f, dry, 0.0f, 10.0f}; }
// Echo used as a pure delay of `ms` (wet only, no feedback).
inline std::vector<float> EchoDelay(float ms) { return {ms, 0.0f, 0.0f, 1.0f, ms}; }

// --- Analysis -----------------------------------------------------------------------
inline bool AllFinite(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}
inline float MaxAbsAll(const std::vector<float>& v) {
  float m = 0;
  for (float x : v) m = std::fabs(x) > m ? std::fabs(x) : m;
  return m;
}
// First frame >= from where |channel c| > thr (or SIZE_MAX).
inline std::size_t FirstAbove(const Fx& h, std::size_t from, float thr, std::uint32_t c = 0) {
  for (std::size_t f = from; f < h.Total(); ++f) {
    if (std::fabs(h.At(f, c)) > thr) return f;
  }
  return static_cast<std::size_t>(-1);
}
inline double Db(double x) { return 20.0 * std::log10(x > 1e-30 ? x : 1e-30); }

// Amplitude of `hz` in channel c of h.all over [from, from + n) (Goertzel, Hann).
inline double ToneOf(const std::vector<float>& x, std::size_t stride, std::size_t ch, std::size_t from, std::size_t n,
                     double hz, double rate = 48000.0) {
  const double w = 6.283185307179586 * hz / rate;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = x[(from + i) * stride + ch] * win;
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}
inline double Tone(const Fx& h, std::size_t from, std::size_t n, double hz, std::uint32_t c = 0) {
  return ToneOf(h.all, h.Ch(), c, from, n, hz);
}

}  // namespace g2

#endif  // SND_CORE_TEST_INDEP_G2_FIXTURE_HPP
