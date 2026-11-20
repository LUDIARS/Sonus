// Independent tests of stage 3 group 4 (voice management, sequences, notifications,
// analysis): own fixture.
//
// Written by the independent tester from the specification (design.md 2.1, 2.5, 4.1, 4.2,
// 4.4, 7; bank-format.md; sonus.h; core.hpp; decisions D-140 .. D-148 of the brief) before
// reading the implementation. The bank builder (test_bank.hpp) and IndepKind
// (indep_fixture.hpp: analytic synth, D-43 checks) are reused; the sample kind is our own:
//
// G4SampleKind (SourceKind::Sample, source rate): plays the asset's PCMF frames exactly,
// honours kStartLoop (loopEnd != 0: [loopStart, loopEnd), else the whole asset) and counts
// completed loops in Playhead::loopCount both while rendering and while virtual
// (AdvanceVirtual, outputFrames * pitch with the same wrap), so core can count N loops
// (D-142) in either state. Seek / Devirtualize ops move the position (taken in at the next
// render / advance of the slot; the playhead is core's). Like the source module's sample
// kind, the asset's own mark (SourceAsset::virtualReturn from ASST.flags, D-140) decides on
// Devirtualize: 1 restart (position 0), 2 kill (the voice reports `ended`). Ends past the last frame.
#ifndef SND_CORE_TEST_INDEP_G4_FIXTURE_HPP
#define SND_CORE_TEST_INDEP_G4_FIXTURE_HPP

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "indep_fixture.hpp"

namespace g4 {

using namespace coretest;
using snd::HashId;
using snd::Id;

class G4SampleKind final : public snd::source::ISourceKind {
 public:
  static constexpr std::uint32_t kCap = 1024;
  snd::SourceKind Kind() const noexcept override { return snd::SourceKind::Sample; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  std::size_t PoolBytes(const snd::source::SourceLimits&) const noexcept override { return 64; }
  bool Init(void*, std::size_t, const snd::source::SourceLimits& l,
            const snd::source::SourceServices&) noexcept override {
    n_ = l.maxVoices < kCap ? l.maxVoices : kCap;
    for (std::uint32_t i = 0; i < kCap; ++i) {
      asset_[i] = nullptr;
      loop_[i] = 0;
      live_[i].store(0);
      seek_[i].store(kNoSeek);
      killed_[i].store(0);
    }
    return true;
  }
  void Shutdown() noexcept override {}
  void Start(const snd::source::SourceStartRequest* r, std::uint32_t n,
             snd::source::SourceStartResult* res) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      res[i] = snd::source::SourceStartResult{snd::kInvalidSlot, 0, 0, 0};
      const snd::SlotIndex s = r[i].voiceSlot;
      if (r[i].asset == nullptr || s >= n_) continue;
      asset_[s] = r[i].asset;
      loop_[s] = (r[i].flags & snd::source::kStartLoop) != 0u ? 1u : 0u;
      startFrame_[s] = r[i].startFrame;
      startClock_[s] = r[i].startClock;
      seek_[s].store(kNoSeek);
      killed_[s].store(0);
      live_[s].store(1);
      res[i].sourceSlot = s;
      starts.fetch_add(1);
      if (loop_[s] != 0u) loopStarts.fetch_add(1);
    }
  }
  void Release(const snd::SlotIndex* s, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (s[i] < n_) live_[s[i]].store(0);
      releases.fetch_add(1);
    }
  }
  void Control(const snd::source::SourceControlOp* ops, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t op = static_cast<std::uint32_t>(ops[i].op);
      if (op < 8u) byOp[op].fetch_add(1);
      const snd::SlotIndex s = ops[i].sourceSlot;
      if (s >= n_) continue;
      if (ops[i].op == snd::source::ControlOp::Seek || ops[i].op == snd::source::ControlOp::Devirtualize) {
        std::uint64_t f = ops[i].frame;
        if (ops[i].op == snd::source::ControlOp::Devirtualize) {
          lastDevirtualizeFrame.store(ops[i].frame);
          const std::uint8_t vr = asset_[s] != nullptr ? asset_[s]->virtualReturn : 0u;
          if (vr == 1u) f = 0;
          if (vr == 2u) killed_[s].store(1);
        }
        seek_[s].store(f);
      }
      if (ops[i].op == snd::source::ControlOp::SetLoop) loop_[s] = 1;
    }
  }
  void QueryReady(const snd::SlotIndex*, std::uint32_t n, std::uint8_t* ready) const noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) ready[i] = 1;
  }
  void RenderBatch(const snd::source::RenderBatch& b, snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const snd::source::RenderItem& it = b.items[i];
      st[i] = snd::VoiceRenderStatus{};
      const snd::SlotIndex s = it.sourceSlot;
      if (s >= n_ || live_[s].load() == 0u) continue;
      if (killed_[s].load() != 0u) {
        st[i].ended = 1;
        continue;
      }
      TakeSeek(s, it.playhead);
      const snd::source::SourceAsset& a = *asset_[s];
      const std::uint32_t ch = a.codec.channels;
      const std::uint64_t total = a.codec.total_frames;
      const float* data = reinterpret_cast<const float*>(a.data);
      std::uint64_t lb = a.loopStart, le = a.loopEnd;
      if (loop_[s] != 0u && le == 0u) {
        lb = 0;
        le = total;
      }
      std::uint64_t pos = it.playhead->frame;
      std::uint32_t written = 0;
      for (std::uint32_t f = 0; f < it.framesRequested; ++f) {
        if (loop_[s] != 0u && le > lb && pos >= le) {
          pos = lb + (pos - le);
          ++it.playhead->loopCount;
        }
        if (pos >= total) {
          st[i].ended = 1;
          break;
        }
        for (std::uint32_t c = 0; c < it.channels; ++c) {
          it.out[c][it.frameOffset + f] = data[pos * ch + (c < ch ? c : ch - 1u)];
        }
        ++pos;
        ++written;
      }
      if (loop_[s] == 0u && pos >= total) st[i].ended = 1;
      it.playhead->frame = pos;
      st[i].framesRendered = written;
    }
  }
  void AdvanceVirtual(const snd::source::AdvanceItem* items, std::uint32_t n,
                      snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      st[i] = snd::VoiceRenderStatus{};
      const snd::SlotIndex s = items[i].sourceSlot;
      if (s >= n_ || live_[s].load() == 0u) continue;
      if (killed_[s].load() != 0u) {
        st[i].ended = 1;
        continue;
      }
      TakeSeek(s, items[i].playhead);
      const snd::source::SourceAsset& a = *asset_[s];
      const std::uint64_t total = a.codec.total_frames;
      std::uint64_t lb = a.loopStart, le = a.loopEnd;
      if (loop_[s] != 0u && le == 0u) {
        lb = 0;
        le = total;
      }
      const double adv = double(items[i].outputFrames) * double(items[i].pitch) * double(a.codec.sample_rate) / 48000.0;
      std::uint64_t pos = items[i].playhead->frame + static_cast<std::uint64_t>(adv + 0.5);
      if (loop_[s] != 0u && le > lb) {
        while (pos >= le) {
          pos = lb + (pos - le);
          ++items[i].playhead->loopCount;
        }
      } else if (pos >= total) {
        st[i].ended = 1;
      }
      items[i].playhead->frame = pos;
      st[i].framesRendered = items[i].outputFrames;
      advances.fetch_add(1);
    }
  }
  void ScheduleBackground(const snd::source::BlockInfo&) noexcept override {}
  std::uint32_t PrimeFrames(const snd::source::SourceAsset&, std::uint64_t) const noexcept override { return 0; }

  std::atomic<std::uint64_t> starts{0}, loopStarts{0}, releases{0}, advances{0};
  std::atomic<std::uint64_t> byOp[8] = {};
  std::atomic<std::uint64_t> lastDevirtualizeFrame{~0ull};

 private:
  static constexpr std::uint64_t kNoSeek = ~0ull;
  void TakeSeek(snd::SlotIndex s, snd::Playhead* ph) noexcept {
    const std::uint64_t f = seek_[s].exchange(kNoSeek);
    if (f != kNoSeek) {
      ph->frame = f;
      ph->fraction = 0;
    }
  }
  std::uint32_t n_ = 0;
  const snd::source::SourceAsset* asset_[kCap] = {};
  std::uint32_t loop_[kCap] = {};
  std::uint64_t startFrame_[kCap] = {};
  std::uint64_t startClock_[kCap] = {};
  std::atomic<std::uint32_t> live_[kCap];
  std::atomic<std::uint64_t> seek_[kCap];
  std::atomic<std::uint32_t> killed_[kCap];
};

// Configuration: indep::Config with the load LOD disabled (2.5: bit identity needs it).
inline snd_config Config(std::uint32_t realVoices = 8, std::uint32_t rtWorkers = 0) {
  snd_config c = indep::Config();
  c.max_real_voices = realVoices;
  c.jobs.realtime_workers = rtWorkers;
  c.tuning.lod_down_blocks = 0xFFFFFFFFu;
  return c;
}

struct End {
  std::uint64_t handle;
  snd_end_reason reason;
  snd_end_detail detail;
  std::uint64_t block;  // rig block index when delivered (Update before that block)
};
struct Marker {
  std::uint64_t handle;
  snd::Id id;
  std::uint64_t clock;
};
struct Err {
  snd_result code;
  std::uint32_t detail;
  std::uint64_t object;
  std::uint64_t block;  // rig block index when delivered
};

struct MarkerTable {
  snd::Id asset = 0;
  std::vector<std::uint64_t> frames;
  std::vector<snd::Id> ids;
};

class Rig {
 public:
  explicit Rig(const snd_config& cfg = Config(), snd::core::CoreLimits limits = {}, snd::IAllocator* allocator = nullptr,
               const MarkerTable* markerTable = nullptr)
      : cfg_(cfg), markers_(markerTable) {
    kinds_.kinds[static_cast<int>(snd::SourceKind::Oscillator)] = &osc;
    kinds_.kinds[static_cast<int>(snd::SourceKind::Sample)] = &sample;
    snd::core::CoreDesc d;
    d.config = &cfg_;
    d.allocator = allocator != nullptr ? allocator : &snd::base::DefaultAllocator();
    d.sources = &kinds_;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    d.limits = limits;
    if (markers_ != nullptr) {
      d.clipMarkers = [](void* u, snd::Id asset) noexcept {
        const MarkerTable* t = static_cast<const MarkerTable*>(u);
        snd::core::CoreDesc::ClipMarkers m{nullptr, nullptr, 0};
        if (asset == t->asset) {
          m.frames = t->frames.data();
          m.ids = t->ids.data();
          m.count = static_cast<std::uint32_t>(t->frames.size());
        }
        return m;
      };
      d.clipMarkersUser = const_cast<MarkerTable*>(markers_);
    }
    created = snd::core::System::Create(d, &sys);
    if (sys != nullptr) {
      snd_callbacks cb{};
      cb.user = this;
      cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail det) {
        Rig* g = static_cast<Rig*>(u);
        g->ends.push_back(End{h.value, r, det, g->block});
      };
      cb.on_marker = [](void* u, snd_playback_handle h, snd_id m, const snd_timing* t) {
        static_cast<Rig*>(u)->markers.push_back(Marker{h.value, m, t != nullptr ? t->clock_sample : ~0ull});
      };
      cb.on_bank_loaded = [](void* u, snd_bank_handle, snd_result r) { static_cast<Rig*>(u)->loads.push_back(r); };
      cb.on_error = [](void* u, snd_result r, std::uint32_t det, std::uint64_t o) {
        Rig* g = static_cast<Rig*>(u);
        g->errors.push_back(Err{r, det, o, g->block});
      };
      sys->SetCallbacks(&cb);
      out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
    }
  }
  ~Rig() { snd::core::System::Destroy(sys); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  void Render() {
    indep::tAudioThread = true;
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
    for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED && loads.empty(); ++i) Step();
    for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) {
      if (!loads.empty() && loads.back() != SND_OK) break;
      Step();
    }
    sys->Update();
    return h;
  }
  std::uint32_t Ch() const { return sys->OutputChannels(); }
  std::uint32_t Frames() const { return sys->BlockFrames(); }
  // Frame index == common clock (the clock starts at 0, every block is kept).
  std::size_t Now() const { return all.size() / Ch(); }
  float At(std::size_t frame, std::uint32_t c = 0) const { return all[frame * Ch() + c]; }
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
  // First frame >= from with |x| > thr (or `to`).
  std::size_t FirstAbove(std::size_t from, std::size_t to, float thr, std::uint32_t c = 0) const {
    for (std::size_t f = from; f < to; ++f) {
      if (std::fabs(At(f, c)) > thr) return f;
    }
    return to;
  }
  // Last frame < to with |x| > thr, plus one (or `from`).
  std::size_t EndAbove(std::size_t from, std::size_t to, float thr, std::uint32_t c = 0) const {
    for (std::size_t f = to; f > from; --f) {
      if (std::fabs(At(f - 1, c)) > thr) return f;
    }
    return from;
  }
  std::size_t CountAbove(std::size_t from, std::size_t to, float thr, std::uint32_t c = 0) const {
    std::size_t n = 0;
    for (std::size_t f = from; f < to; ++f) n += std::fabs(At(f, c)) > thr ? 1u : 0u;
    return n;
  }
  std::uint32_t CountEnds(std::uint64_t h) const {
    std::uint32_t n = 0;
    for (const End& e : ends) n += e.handle == h ? 1u : 0u;
    return n;
  }
  const End* EndOf(std::uint64_t h) const {
    for (const End& e : ends) {
      if (e.handle == h) return &e;
    }
    return nullptr;
  }
  bool Ended(std::uint64_t h, snd_end_reason r, snd_end_detail d = 0xFFFFFFFFu) const {
    for (const End& e : ends) {
      if (e.handle == h && e.reason == r && (d == 0xFFFFFFFFu || e.detail == d)) return true;
    }
    return false;
  }

  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  indep::IndepKind osc;
  G4SampleKind sample;
  snd::core::System* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  std::vector<End> ends;
  std::vector<Marker> markers;
  std::vector<Err> errors;
  std::vector<snd_result> loads;
  std::vector<float> out, all;
  std::uint64_t block = 0;

 private:
  snd_config cfg_;
  const MarkerTable* markers_;
  snd::source::SourceKindSet kinds_{};
};

inline constexpr float kC = 0.70710678f;  // constant-power centre of a mono clip on a stereo bus

// Bank helper: buses Sfx (row 0) -> Master (row 1, stereo); assets and synths by index.
struct B {
  TestBank b;
  B() {
    b.buses.push_back(TBus{HashId("Sfx"), 1u, 0, 0, 0.0f, {}, {}});
    b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  }
  std::uint32_t Synth(float f) {
    b.synths.push_back(TSynth{0, 0, 1, f});
    return static_cast<std::uint32_t>(b.synths.size() - 1u);
  }
  std::uint32_t Asset(const char* name, std::vector<float> data, std::uint64_t loopStart = 0, std::uint64_t loopEnd = 0,
                      std::uint32_t flags = 0, std::uint32_t channels = 1) {
    TAsset a;
    a.id = HashId(name);
    a.channels = channels;
    a.data = std::move(data);
    a.loopStart = loopStart;
    a.loopEnd = loopEnd;
    a.flags = flags;
    b.assets.push_back(a);
    return static_cast<std::uint32_t>(b.assets.size() - 1u);
  }
  static TClip SynthClip(std::uint32_t synth, float volumeDb = 0) {
    TClip c;
    c.kind = 2;
    c.target = synth;
    c.volumeDb = volumeDb;
    return c;
  }
  static TClip AssetClip(std::uint32_t asset, float volumeDb = 0) {
    TClip c;
    c.kind = 0;
    c.target = asset;
    c.volumeDb = volumeDb;
    return c;
  }
  // Event with one track holding `clips`.
  TEvent& Event(const char* name, std::vector<TClip> clips, std::uint32_t sequence = 0, std::uint32_t bus = 0) {
    TEvent e;
    e.id = HashId(name);
    e.sequence = sequence;
    e.bus = bus;
    TTrack t;
    t.clips = std::move(clips);
    e.tracks.push_back(t);
    b.events.push_back(e);
    return b.events.back();
  }
  std::vector<std::uint8_t> Build(const char* id) const { return b.Build(HashId(id)); }
};

inline snd_emitter_handle NoEm() { return snd_emitter_handle{0}; }
inline snd_when AtClock(std::uint64_t clock) {
  snd_when w{};
  w.kind = SND_WHEN_CLOCK_SAMPLE;
  w.value = clock;
  return w;
}

}  // namespace g4

#endif  // SND_CORE_TEST_INDEP_G4_FIXTURE_HPP
