// Independent tests of core (stage 2): own source kind and fixture.
//
// Written by the independent tester from the specification (design.md 2-5,
// 10.3, 14; CONTRACTS.md; the internal contract headers) and core.hpp only.
// The bank builder (test_bank.hpp) is a thin wrapper over the build tool's
// writer and is reused; the source kinds of test_sources.hpp are NOT used.
//
// IndepKind (SourceKind::Oscillator, renders at the output rate). The SYNP
// frequency selects an analytic signal, independent of the core under test:
//   0 < f < 1000         DC of level f / 1000, endless
//   f == 1000            counter: value = ((frame % 4096) + 1) / 4096, endless
//   2000 <= f            DC 1.0 for (f - 2000) frames, then `ended`
// `frame` is the playhead the kind itself advances (RenderItem::playhead,
// AdvanceItem::playhead), so the output reveals the playhead.
//
// The kind also checks the D-43 / A1-14 rules from the source's side:
//   overlap      an item whose real slot or source slot is already inside a
//                running RenderBatch (two renders of the same render state)
//   discont      a render or advance that does not start where the previous
//                one of the same source slot stopped (lost or doubled progress)
//   doubleBlock  the same source slot rendered twice for the same block
//   releasedBusy Release of a source slot that a chunk is rendering
//   deadRender   RenderBatch of a released (or never started) source slot
//   controlBusy  Control (pause, virtualise, ...) of a source slot that a chunk is rendering
// A test can slow down RenderBatch on RT workers (not on the audio thread)
// to produce late chunks, or close the gate for a late chunk without timing:
// while the gate is closed, an RT worker that enters RenderBatch of a
// parallel batch stays there until OpenGate(), and the audio thread, in a
// parallel batch, stays in its RenderBatch until a worker has entered one of
// the same batch. A parallel block rendered with the gate closed therefore has
// a chunk that is still running at its deadline, and no block is quiescent
// until the gate is opened. The waits are bounded (gateTimeouts) only as a
// safety net.
#ifndef SND_CORE_TEST_INDEP_FIXTURE_HPP
#define SND_CORE_TEST_INDEP_FIXTURE_HPP

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/core/core.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"
#include "test_bank.hpp"

namespace indep {

using namespace coretest;
using snd::SlotIndex;  // TestBank & co (bank writer wrapper)

inline thread_local bool tAudioThread = false;  // set by the test thread that renders

struct Inst {
  std::uint32_t mode;  // 0 DC endless, 1 counter, 2 finite DC
  float level;
  std::uint64_t length;
  std::uint32_t live;
};

class IndepKind final : public snd::source::ISourceKind {
 public:
  explicit IndepKind(std::uint32_t maxSlots = 2048)
      : cap_(maxSlots),
        inUseSrc_(new std::atomic<std::uint32_t>[maxSlots]),
        inUseReal_(new std::atomic<std::uint32_t>[maxSlots]),
        lastExit_(new std::atomic<std::uint64_t>[maxSlots]),
        lastBlock_(new std::atomic<std::uint64_t>[maxSlots]),
        framesRendered_(new std::atomic<std::uint64_t>[maxSlots]),
        live_(new std::atomic<std::uint32_t>[maxSlots]),
        startClock_(new std::atomic<std::uint64_t>[maxSlots]) {
    for (std::uint32_t i = 0; i < cap_; ++i) {
      inUseSrc_[i].store(0);
      inUseReal_[i].store(0);
      lastExit_[i].store(0);
      lastBlock_[i].store(0);
      framesRendered_[i].store(0);
      live_[i].store(0);
      startClock_[i].store(0);
    }
  }
  snd::SourceKind Kind() const noexcept override { return snd::SourceKind::Oscillator; }
  bool RendersAtOutputRate() const noexcept override { return true; }
  std::size_t PoolBytes(const snd::source::SourceLimits& l) const noexcept override {
    return sizeof(Inst) * l.maxVoices;
  }
  bool Init(void* pool, std::size_t, const snd::source::SourceLimits& l,
            const snd::source::SourceServices& s) noexcept override {
    inst_ = static_cast<Inst*>(pool);
    n_ = l.maxVoices < cap_ ? l.maxVoices : cap_;
    services_ = s;
    for (std::uint32_t i = 0; i < n_; ++i) inst_[i] = Inst{0, 0, 0, 0};
    return true;
  }
  void Shutdown() noexcept override {}
  void Start(const snd::source::SourceStartRequest* r, std::uint32_t n,
             snd::source::SourceStartResult* res) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      res[i] = snd::source::SourceStartResult{snd::kInvalidSlot, 0, 0, 0};
      const auto* p = static_cast<const snd::source::OscillatorParams*>(r[i].params);
      const SlotIndex s = r[i].voiceSlot;
      if (s >= n_) continue;
      if (live_[s].load() != 0u) liveStart.fetch_add(1);  // core reused a live source slot
      const float f = p != nullptr ? p->frequencyHz : 500.0f;
      Inst in{0, 0, 0, 1};
      if (f < 1000.0f) {
        in.mode = 0;
        in.level = f / 1000.0f;
      } else if (f < 2000.0f) {
        in.mode = 1;
        in.level = 1.0f;
      } else {
        in.mode = 2;
        in.level = 1.0f;
        in.length = static_cast<std::uint64_t>(f - 2000.0f);
      }
      inst_[s] = in;
      lastExit_[s].store(r[i].startFrame);
      lastBlock_[s].store(0);
      framesRendered_[s].store(0);
      live_[s].store(1);
      startClock_[s].store(r[i].startClock);
      res[i].sourceSlot = s;
      starts.fetch_add(1);
      startedSinceLastPost.fetch_add(1);
    }
  }
  void Release(const SlotIndex* s, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (s[i] >= n_) continue;
      if (inUseSrc_[s[i]].load(std::memory_order_acquire) != 0u) releasedBusy.fetch_add(1);
      live_[s[i]].store(0);
      releases.fetch_add(1);
    }
  }
  // A Control (pause / virtualise ...) changes the source instance: never while a chunk renders it (D-43).
  void Control(const snd::source::SourceControlOp* ops, std::uint32_t n) noexcept override {
    controls.fetch_add(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      if (static_cast<std::uint32_t>(ops[i].op) < 8u) controlsByOp[static_cast<std::uint32_t>(ops[i].op)].fetch_add(1);
      if (ops[i].sourceSlot < n_ && inUseSrc_[ops[i].sourceSlot].load(std::memory_order_acquire) != 0u) {
        controlBusy.fetch_add(1);
        controlBusyOps.fetch_or(1u << static_cast<std::uint32_t>(ops[i].op));
      }
    }
  }
  void QueryReady(const SlotIndex*, std::uint32_t n, std::uint8_t* ready) const noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) ready[i] = 1;
  }
  static float Value(const Inst& in, std::uint64_t frame) noexcept {
    if (in.mode == 1) return static_cast<float>((frame % 4096u) + 1u) / 4096.0f * in.level;
    return in.level;
  }
  void RenderBatch(const snd::source::RenderBatch& b, snd::VoiceRenderStatus* st) noexcept override {
    batches.fetch_add(1);
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const auto& it = b.items[i];
      if (it.sourceSlot < n_ && inUseSrc_[it.sourceSlot].exchange(1u, std::memory_order_acq_rel) != 0u) overlap.fetch_add(1);
      if (it.realSlot < cap_ && inUseReal_[it.realSlot].exchange(1u, std::memory_order_acq_rel) != 0u) overlap.fetch_add(1);
    }
    if (b.chunkGeneration != 0u) Gate(b.chunkGeneration);
    const std::uint64_t callerSlow = callerSlowNs.load();
    if (callerSlow != 0u && tAudioThread) {  // lets the workers claim chunks at all
      const std::uint64_t until = snd::platform::MonotonicNanoseconds() + callerSlow;
      while (snd::platform::MonotonicNanoseconds() < until) {
      }
    }
    const std::uint64_t slow = slowNs.load();
    if (slow != 0u && !tAudioThread && (slowBlockMask.load() == 0u || (b.block.index % slowBlockMask.load()) == 0u)) {
      const std::uint64_t until = snd::platform::MonotonicNanoseconds() + slow;
      while (snd::platform::MonotonicNanoseconds() < until) {
      }
    }
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const auto& it = b.items[i];
      st[i] = snd::VoiceRenderStatus{};
      if (it.sourceSlot >= n_ || live_[it.sourceSlot].load() == 0u) {
        deadRender.fetch_add(1);
        continue;
      }
      const Inst& in = inst_[it.sourceSlot];
      std::uint64_t frame = it.playhead->frame;
      if (frame != lastExit_[it.sourceSlot].load()) discont.fetch_add(1);
      if (lastBlock_[it.sourceSlot].exchange(b.block.index + 1u) == b.block.index + 1u) doubleBlock.fetch_add(1);
      // Keeps time: an endless voice started before this block is at clock - start
      // (only meaningful without pauses; not part of ViolationSum).
      const std::uint64_t st0 = startClock_[it.sourceSlot].load();
      if (it.frameOffset == 0u && st0 <= b.block.clock && frame != b.block.clock - st0) timeLag.fetch_add(1);
      std::uint32_t written = 0;
      for (std::uint32_t f = 0; f < it.framesRequested; ++f) {
        if (in.mode == 2 && frame >= in.length) {
          st[i].ended = 1;
          break;
        }
        const float v = Value(in, frame);
        for (std::uint32_t c = 0; c < it.channels; ++c) it.out[c][it.frameOffset + f] = v;
        ++frame;
        ++written;
      }
      if (in.mode == 2 && frame >= in.length) st[i].ended = 1;
      it.playhead->frame = frame;
      lastExit_[it.sourceSlot].store(frame);
      framesRendered_[it.sourceSlot].fetch_add(written);
      st[i].framesRendered = written;
    }
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const auto& it = b.items[i];
      if (it.sourceSlot < n_) inUseSrc_[it.sourceSlot].store(0u, std::memory_order_release);
      if (it.realSlot < cap_) inUseReal_[it.realSlot].store(0u, std::memory_order_release);
    }
  }
  void AdvanceVirtual(const snd::source::AdvanceItem* items, std::uint32_t n,
                      snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      st[i] = snd::VoiceRenderStatus{};
      const SlotIndex s = items[i].sourceSlot;
      if (s >= n_ || live_[s].load() == 0u) {
        deadRender.fetch_add(1);
        continue;
      }
      if (inUseSrc_[s].load(std::memory_order_acquire) != 0u) overlap.fetch_add(1);
      if (items[i].playhead->frame != lastExit_[s].load()) discont.fetch_add(1);
      items[i].playhead->frame += items[i].outputFrames;
      lastExit_[s].store(items[i].playhead->frame);
      if (inst_[s].mode == 2 && items[i].playhead->frame >= inst_[s].length) st[i].ended = 1;
      st[i].framesRendered = items[i].outputFrames;
      advances.fetch_add(1);
    }
  }
  // Posts `postJobs` jobs per block (set by the test) through SourceServices (D-08).
  void ScheduleBackground(const snd::source::BlockInfo&) noexcept override {
    std::uint32_t k = postJobs.exchange(0u);
    for (; k > 0u; --k) {
      snd::JobDesc j{};
      j.fn = &JobFn;
      j.data = this;
      if (services_.postJob != nullptr && services_.postJob(services_.jobCtx, j, snd::JobLane::Prefetch)) {
        posted.fetch_add(1);
      }
    }
  }
  std::uint32_t PrimeFrames(const snd::source::SourceAsset&, std::uint64_t) const noexcept override { return 0; }

  static void SND_CALL JobFn(void* p) { static_cast<IndepKind*>(p)->jobsRun.fetch_add(1); }

  // Test thread: closes / opens the gate (see the file comment).
  void CloseGate() { gateClosed.store(1u); }
  void OpenGate() {
    gateClosed.store(0u);
    gateEpoch.fetch_add(1u);
  }

  std::uint64_t FramesRendered(SlotIndex s) const { return framesRendered_[s].load(); }
  std::uint64_t ViolationSum() const {
    return overlap.load() + discont.load() + doubleBlock.load() + releasedBusy.load() + deadRender.load() +
           liveStart.load() + controlBusy.load();
  }

  std::atomic<std::uint64_t> batches{0}, starts{0}, releases{0}, controls{0}, advances{0};
  std::atomic<std::uint64_t> overlap{0}, discont{0}, doubleBlock{0}, releasedBusy{0}, deadRender{0}, liveStart{0},
      controlBusy{0};
  std::atomic<std::uint64_t> slowNs{0};
  std::atomic<std::uint64_t> callerSlowNs{0};       // per batch on the audio thread
  std::atomic<std::uint32_t> controlBusyOps{0};     // bit per ControlOp seen while busy
  std::atomic<std::uint64_t> timeLag{0};            // renders whose playhead is not clock - start
  std::atomic<std::uint64_t> controlsByOp[8] = {};  // per snd::source::ControlOp
  std::atomic<std::uint64_t> slowBlockMask{0};  // 0: every block, else blocks with index % mask == 0
  std::atomic<std::uint32_t> postJobs{0};
  std::atomic<std::uint64_t> posted{0}, jobsRun{0}, startedSinceLastPost{0};
  std::atomic<std::uint32_t> gateClosed{0}, gateEpoch{0};
  std::atomic<std::uint32_t> gateWorkerGeneration{0};  // newest parallel batch a worker entered while gated
  std::atomic<std::uint32_t> gateTimeouts{0};          // safety bound hit: never expected

 private:
  static constexpr std::uint64_t kGateLimitNs = 30000000000ull;  // 30 s
  void Gate(std::uint32_t generation) noexcept {
    if (!tAudioThread) {
      // Epoch first (seq_cst): if the gate is still seen closed, OpenGate() moves this epoch on later.
      const std::uint32_t epoch = gateEpoch.load();
      if (gateClosed.load() == 0u) return;
      std::uint32_t seen = gateWorkerGeneration.load();
      while (seen < generation && !gateWorkerGeneration.compare_exchange_weak(seen, generation)) {
      }
      const std::uint64_t limit = snd::platform::MonotonicNanoseconds() + kGateLimitNs;
      while (gateEpoch.load() == epoch) {
        if (snd::platform::MonotonicNanoseconds() > limit) {
          gateTimeouts.fetch_add(1u);
          break;
        }
        std::this_thread::yield();
      }
      return;
    }
    if (gateClosed.load() == 0u) return;
    const std::uint64_t limit = snd::platform::MonotonicNanoseconds() + kGateLimitNs;
    while (gateWorkerGeneration.load() < generation) {
      if (snd::platform::MonotonicNanoseconds() > limit) {
        gateTimeouts.fetch_add(1u);
        break;
      }
      std::this_thread::yield();
    }
  }
  Inst* inst_ = nullptr;
  std::uint32_t n_ = 0;
  std::uint32_t cap_;
  snd::source::SourceServices services_{};
  std::unique_ptr<std::atomic<std::uint32_t>[]> inUseSrc_, inUseReal_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> lastExit_, lastBlock_, framesRendered_;
  std::unique_ptr<std::atomic<std::uint32_t>[]> live_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> startClock_;
};

inline snd_config Config() {
  snd_config c;
  snd_config_init(&c);
  c.max_real_voices = 8;
  c.max_virtual_voices = 128;
  c.fade_reserve_voices = 4;
  c.max_playbacks = 128;
  c.max_emitters = 16;
  c.max_transports = 4;
  c.max_reservations = 64;
  c.max_streams = 4;
  c.max_banks = 8;
  c.max_user_buffers = 4;
  c.max_global_params = 32;
  c.param_overflow_blocks = 8;
  c.event_ring_capacity = 256;
  c.max_registered_threads = 6;
  c.commands.arena_bytes = 16u << 10;
  c.commands.reserve_bytes = 2u << 10;
  c.commands.overflow_param_slots = 16;
  c.memory.frame_arena_bytes = 256u << 10;
  c.memory.thread_arena_bytes = 256u << 10;
  c.jobs.realtime_workers = 0;
  c.jobs.voices_per_chunk = 4;
  c.max_voice_channels = 2;
  return c;
}

struct End {
  std::uint64_t handle;
  snd_end_reason reason;
  snd_end_detail detail;
};
struct Err {
  snd_result code;
  std::uint32_t detail;
  std::uint64_t object;
};

class Fx {
 public:
  explicit Fx(const snd_config& cfg = Config(), snd::core::CoreLimits limits = {}, snd::IAllocator* allocator = nullptr,
              std::uint32_t claimDelayUs = 0)
      : cfg_(cfg) {
    kinds_.kinds[static_cast<int>(snd::SourceKind::Oscillator)] = &kind;
    snd::core::CoreDesc d;
    d.config = &cfg_;
    d.allocator = allocator != nullptr ? allocator : &snd::base::DefaultAllocator();
    d.sources = &kinds_;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    d.rtDebugClaimDelayMicroseconds = claimDelayUs;
    d.limits = limits;
    created = snd::core::System::Create(d, &sys);
    if (sys != nullptr) {
      snd_callbacks cb{};
      cb.user = this;
      cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail det) {
        static_cast<Fx*>(u)->ends.push_back(End{h.value, r, det});
      };
      cb.on_bank_loaded = [](void* u, snd_bank_handle, snd_result r) { static_cast<Fx*>(u)->loads.push_back(r); };
      cb.on_error = [](void* u, snd_result r, std::uint32_t det, std::uint64_t o) {
        static_cast<Fx*>(u)->errors.push_back(Err{r, det, o});
      };
      sys->SetCallbacks(&cb);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~Fx() { snd::core::System::Destroy(sys); }
  Fx(const Fx&) = delete;
  Fx& operator=(const Fx&) = delete;

  void Render() {
    tAudioThread = true;
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
    ++block;
  }
  void Step() {
    sys->Update();
    Render();
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
  std::uint32_t Ch() const { return sys->OutputChannels(); }
  std::uint32_t Frames() const { return sys->BlockFrames(); }
  // Sample of the stream since construction (frame index == common clock, the
  // clock starts at 0 and every block is kept).
  float At(std::size_t frame, std::uint32_t c = 0) const { return all[frame * Ch() + c]; }
  std::size_t Total() const { return all.size() / Ch(); }
  float Mean(std::size_t from, std::size_t to, std::uint32_t c = 0) const {
    double s = 0;
    for (std::size_t f = from; f < to; ++f) s += At(f, c);
    return static_cast<float>(s / double(to - from));
  }
  float MaxAbs(std::size_t from, std::size_t to, std::uint32_t c = 0) const {
    float m = 0;
    for (std::size_t f = from; f < to; ++f) m = std::fabs(At(f, c)) > m ? std::fabs(At(f, c)) : m;
    return m;
  }
  // Mean of the last rendered block.
  float LastMean(std::uint32_t c = 0) const { return Mean(Total() - Frames(), Total(), c); }
  std::uint32_t CountEnds(std::uint64_t h) const {
    std::uint32_t n = 0;
    for (const End& e : ends) n += e.handle == h ? 1u : 0u;
    return n;
  }
  bool Ended(std::uint64_t h, snd_end_reason r, snd_end_detail d = 0xFFFFFFFFu) const {
    for (const End& e : ends) {
      if (e.handle == h && e.reason == r && (d == 0xFFFFFFFFu || e.detail == d)) return true;
    }
    return false;
  }

  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  IndepKind kind;
  snd::core::System* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  std::vector<End> ends;
  std::vector<Err> errors;
  std::vector<snd_result> loads;
  std::vector<float> out, all;
  std::uint64_t block = 0;

 private:
  snd_config cfg_;
  snd::source::SourceKindSet kinds_{};
};

// Bank helper: master (2 ch) <- "Sfx"; synth clips through IndepKind.
struct Bank {
  TestBank b;
  Bank() {
    b.buses.push_back(TBus{snd::HashId("Sfx"), 1u, 0, 0, 0.0f, {}, {}});
    b.buses.push_back(TBus{snd::HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  }
  std::uint32_t Synth(float f) {
    b.synths.push_back(TSynth{0, 0, 1, f});
    return static_cast<std::uint32_t>(b.synths.size() - 1u);
  }
  TEvent& Event(const char* name, float f, std::uint32_t priority = 128, std::uint32_t bus = 0,
                std::uint32_t category = kNone, float volumeDb = 0.0f) {
    const std::uint32_t s = Synth(f);
    TEvent e;
    e.id = snd::HashId(name);
    e.priority = priority;
    e.bus = bus;
    e.category = category;
    TTrack t;
    TClip c;
    c.kind = 2;
    c.target = s;
    c.volumeDb = volumeDb;
    t.clips.push_back(c);
    e.tracks.push_back(t);
    b.events.push_back(e);
    return b.events.back();
  }
  std::vector<std::uint8_t> Build(const char* id, std::uint32_t lang = 0) const { return b.Build(snd::HashId(id), lang); }
};

inline constexpr float kCenter = 0.70710678f;  // constant-power centre (checked by a calibration test)
inline snd_emitter_handle NoEmitter() { return snd_emitter_handle{0}; }
inline snd_when AtClock(std::uint64_t clock) {
  snd_when w{};
  w.kind = SND_WHEN_CLOCK_SAMPLE;
  w.value = clock;
  return w;
}

}  // namespace indep

#endif  // SND_CORE_TEST_INDEP_FIXTURE_HPP
