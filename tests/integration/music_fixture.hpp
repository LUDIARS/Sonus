// Fixture of the music integration tests (stage 4): project (JSON + WAV) ->
// buildtool -> bank -> core (DirectFileSystem, deterministic jobs) with the
// music system (snd::music::MusicSystem) -> rendered output.
//
// The source kind is a Sample kind written for these tests (MusicKind): it
// plays resident PCMF exactly (no resampling at 48 kHz), logs every Start
// (clock, frame, asset), reports a configurable prime time, and can starve
// chosen assets during a clock range (sound priority: the playhead holds;
// time priority: it moves on) to exercise sync-group stalls (8.4).
// Test material is mono, so both output channels carry x * kCenter
// (constant-power centre pan).
#ifndef SND_TESTS_INTEGRATION_MUSIC_FIXTURE_HPP
#define SND_TESTS_INTEGRATION_MUSIC_FIXTURE_HPP

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mini_project.hpp"
#include "snd/base/allocator.hpp"
#include "snd/core/core.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/music/music_system.hpp"
#include "snd/test/test.hpp"

namespace mt {

namespace bt = snd::buildtool;
using snd::HashId;

inline constexpr float kCenter = 0.70710678f;
inline constexpr std::uint32_t kRate = 48000;
inline constexpr std::uint32_t kBeat = 24000;  // 120 BPM at 48 kHz
inline constexpr std::uint32_t kBar = 4 * kBeat;

struct StartLog {
  std::uint64_t clock;
  std::uint64_t frame;
  snd::Id asset;
  std::uint32_t voice;
};

struct KindInstance {
  const snd::source::SourceAsset* asset;
  std::uint32_t live;
  std::uint32_t loop;
};

class MusicKind final : public snd::source::ISourceKind {
 public:
  static constexpr std::uint32_t kMaxLog = 512;
  snd::SourceKind Kind() const noexcept override { return snd::SourceKind::Sample; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  std::size_t PoolBytes(const snd::source::SourceLimits& l) const noexcept override {
    return sizeof(KindInstance) * l.maxVoices;
  }
  bool Init(void* pool, std::size_t, const snd::source::SourceLimits& l,
            const snd::source::SourceServices&) noexcept override {
    inst_ = static_cast<KindInstance*>(pool);
    cap_ = l.maxVoices;
    for (std::uint32_t i = 0; i < cap_; ++i) inst_[i] = KindInstance{nullptr, 0, 0};
    return true;
  }
  void Shutdown() noexcept override {}
  void Start(const snd::source::SourceStartRequest* r, std::uint32_t n,
             snd::source::SourceStartResult* res) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      res[i] = snd::source::SourceStartResult{snd::kInvalidSlot, 0, 0, 0};
      if (r[i].asset == nullptr || r[i].voiceSlot >= cap_) continue;
      inst_[r[i].voiceSlot] = KindInstance{r[i].asset, 1u, (r[i].flags & snd::source::kStartLoop) != 0u ? 1u : 0u};
      res[i].sourceSlot = r[i].voiceSlot;
      const std::uint32_t k = logCount.load(std::memory_order_relaxed);
      if (k < kMaxLog) {
        log[k] = StartLog{r[i].startClock, r[i].startFrame, r[i].asset->assetId, r[i].voiceSlot};
        logCount.store(k + 1u, std::memory_order_release);
      }
      syncGroups[r[i].voiceSlot % 64u] = r[i].syncGroup;
    }
  }
  void Release(const snd::SlotIndex* s, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (s[i] < cap_) inst_[s[i]].live = 0;
    }
  }
  void Control(const snd::source::SourceControlOp*, std::uint32_t) noexcept override {}
  void QueryReady(const snd::SlotIndex*, std::uint32_t n, std::uint8_t* ready) const noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) ready[i] = 1;
  }
  void RenderBatch(const snd::source::RenderBatch& b, snd::VoiceRenderStatus* st) noexcept override {
    const std::uint64_t from = stallFrom.load(std::memory_order_relaxed);
    const std::uint64_t to = stallTo.load(std::memory_order_relaxed);
    const snd::Id only = stallAsset.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const snd::source::RenderItem& it = b.items[i];
      st[i] = snd::VoiceRenderStatus{};
      if (it.sourceSlot >= cap_ || inst_[it.sourceSlot].live == 0u) continue;
      const KindInstance& s = inst_[it.sourceSlot];
      const snd::source::SourceAsset& a = *s.asset;
      const std::uint32_t ch = a.codec.channels;
      const std::uint64_t total = a.codec.total_frames;
      const float* data = reinterpret_cast<const float*>(a.data);
      const bool stalled = b.block.clock >= from && b.block.clock < to && (only == 0u || only == a.assetId);
      if (stalled) {  // starved for the whole block (8.4 / 6.3)
        st[i].starved = 1;
        st[i].framesRendered = 0;
        if (a.stallPolicy == 0u) it.playhead->frame += it.framesRequested;  // time priority: skip
        continue;
      }
      std::uint64_t pos = it.playhead->frame;
      std::uint32_t written = 0;
      for (std::uint32_t f = 0; f < it.framesRequested; ++f) {
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
      if (pos >= total) st[i].ended = 1;
      it.playhead->frame = pos;
      st[i].framesRendered = written;
    }
  }
  void AdvanceVirtual(const snd::source::AdvanceItem* items, std::uint32_t n,
                      snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      st[i] = snd::VoiceRenderStatus{};
      items[i].playhead->frame += static_cast<std::uint64_t>(items[i].outputFrames * items[i].pitch);
      if (items[i].playhead->frame >= inst_[items[i].sourceSlot].asset->codec.total_frames) st[i].ended = 1;
    }
  }
  void ScheduleBackground(const snd::source::BlockInfo&) noexcept override {}
  std::uint32_t PrimeFrames(const snd::source::SourceAsset& a, std::uint64_t) const noexcept override {
    const snd::Id only = primeAsset.load(std::memory_order_relaxed);
    return (only == 0u || only == a.assetId) ? primeFrames.load(std::memory_order_relaxed) : 0u;
  }

  std::vector<StartLog> Starts() const {
    std::vector<StartLog> v;
    const std::uint32_t n = logCount.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < n; ++i) v.push_back(log[i]);
    return v;
  }

  std::atomic<std::uint32_t> primeFrames{0};
  std::atomic<snd::Id> primeAsset{0};
  std::atomic<std::uint64_t> stallFrom{0}, stallTo{0};
  std::atomic<snd::Id> stallAsset{0};
  std::atomic<std::uint32_t> logCount{0};
  StartLog log[kMaxLog] = {};
  std::uint32_t syncGroups[64] = {};

 private:
  KindInstance* inst_ = nullptr;
  std::uint32_t cap_ = 0;
};

inline snd_config Config(std::uint32_t rtWorkers = 0) {
  snd_config c;
  snd_config_init(&c);
  c.max_real_voices = 32;
  c.max_virtual_voices = 64;
  c.fade_reserve_voices = 8;
  c.max_playbacks = 64;
  c.max_emitters = 8;
  c.max_transports = 4;
  c.max_reservations = 32;
  c.max_streams = 4;
  c.max_banks = 4;
  c.max_user_buffers = 2;
  c.max_global_params = 16;
  c.param_overflow_blocks = 8;
  c.event_ring_capacity = 4096;
  c.max_registered_threads = 4;
  c.commands.arena_bytes = 16u << 10;
  c.commands.reserve_bytes = 2u << 10;
  c.commands.overflow_param_slots = 16;
  c.memory.frame_arena_bytes = 128u << 10;
  c.memory.thread_arena_bytes = 256u << 10;
  c.jobs.realtime_workers = rtWorkers;
  c.jobs.voices_per_chunk = 1;
  c.max_voice_channels = 2;
  return c;
}

struct Beat {
  std::uint64_t transport;
  std::int32_t bar;
  std::int32_t beat;
  std::uint64_t clock;
};
struct Marker {
  std::uint64_t playback;
  snd::Id id;
  std::uint64_t clock;
};
struct ErrorRec {
  snd_result code;
  std::uint32_t detail;
  std::uint64_t object;
};
struct EndRec {
  std::uint64_t handle;
  snd_end_reason reason;
  snd_end_detail detail;
};

struct Events {
  std::vector<Beat> beats;
  std::vector<Marker> markers;
  std::vector<ErrorRec> errors;
  std::vector<EndRec> ends;
};

class Rig {
 public:
  // `musicOverride`: an IMusicSystem given to core instead of `music` (e.g. a wrapper around it).
  explicit Rig(const snd_config& cfg = Config(), snd::IAllocator* allocator = nullptr,
               snd::music::IMusicSystem* musicOverride = nullptr)
      : cfg_(cfg) {
    kinds_.kinds[static_cast<int>(snd::SourceKind::Sample)] = &kind;
    snd::core::CoreDesc d;
    d.config = &cfg_;
    d.allocator = allocator != nullptr ? allocator : &snd::base::DefaultAllocator();
    d.sources = &kinds_;
    d.music = musicOverride != nullptr ? musicOverride : &music;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    created = snd::core::System::Create(d, &sys);
    if (sys == nullptr) return;
    snd_callbacks cb{};
    cb.user = &ev;
    cb.on_beat = [](void* u, snd_transport_handle t, const snd_musical_time* p, const snd_timing* tm) {
      static_cast<Events*>(u)->beats.push_back(Beat{t.value, p->bar, p->beat, tm->clock_sample});
    };
    cb.on_marker = [](void* u, snd_playback_handle p, snd::Id id, const snd_timing* tm) {
      static_cast<Events*>(u)->markers.push_back(Marker{p.value, id, tm->clock_sample});
    };
    cb.on_error = [](void* u, snd_result r, std::uint32_t det, std::uint64_t obj) {
      static_cast<Events*>(u)->errors.push_back(ErrorRec{r, det, obj});
    };
    cb.on_end = [](void* u, snd_playback_handle h, snd_end_reason r, snd_end_detail det) {
      static_cast<Events*>(u)->ends.push_back(EndRec{h.value, r, det});
    };
    sys->SetCallbacks(&cb);
    out_.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
  }
  ~Rig() { snd::core::System::Destroy(sys); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  bool Load(const std::vector<std::uint8_t>& bytes, const std::string& path = "Main.sndb") {
    fs.AddFile(path, bytes);
    const snd_bank_handle h = sys->LoadBankAsync(path.c_str());
    for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) Step();
    sys->Update();
    all.clear();
    base = Clock();
    return sys->GetBankState(h) == SND_BANK_STATE_LOADED;
  }
  void Step() {
    sys->Update();
    sys->RenderBlock(out_.data());
    ++blocks_;
    all.insert(all.end(), out_.begin(), out_.end());
  }
  void Steps(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Step();
  }
  // Renders until the common clock reaches `clock` (block granularity).
  void RunTo(std::uint64_t clock) {
    while (Clock() < clock) Step();
  }
  // Common clock of the next block to render.
  std::uint64_t Clock() const { return blocks_ * sys->BlockFrames(); }
  // Output sample (left) at common clock `clock`, normalised by the centre pan.
  float At(std::uint64_t clock) const {
    const std::size_t f = static_cast<std::size_t>(clock - base);
    return f < all.size() / 2 ? all[f * 2] / kCenter : 0.0f;
  }
  std::uint64_t Rendered() const { return base + all.size() / 2; }

  snd::music::MusicSystem music;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  MusicKind kind;
  snd::core::System* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  Events ev;
  std::vector<float> all;  // interleaved stereo since Load()
  std::uint64_t base = 0;  // clock of all[0]

 private:
  snd_config cfg_;
  snd::source::SourceKindSet kinds_{};
  std::vector<float> out_;
  std::uint64_t blocks_ = 0;
};

// --- project helpers ---------------------------------------------------------------
// Mono material: `level` everywhere, 1.0 clicks at multiples of `clickEvery`
// frames counted from `clickFrom` (0 = no clicks).
inline bt::PcmData Material(std::uint64_t frames, float level, std::uint64_t clickEvery = 0,
                            std::uint64_t clickFrom = 0) {
  bt::PcmData p;
  p.channels = 1;
  p.sampleRate = kRate;
  p.frames = frames;
  p.samples.assign(static_cast<std::size_t>(frames), level);
  if (clickEvery != 0u) {
    for (std::uint64_t f = clickFrom; f < frames; f += clickEvery) p.samples[static_cast<std::size_t>(f)] = 1.0f;
  }
  return p;
}

class MusicProject {
 public:
  explicit MusicProject(const std::string& name) : p_(name) {
    p_.Add("banks/Main.json", "bank", "Main");
    p_.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  }
  // Mono PCMF resident asset. `extra`: more asset members (e.g. "\"stallPolicy\": \"sound\"").
  void Asset(const std::string& name, const bt::PcmData& pcm, const std::string& extra = "") {
    p_.Wav("audio/" + name + ".wav", pcm, bt::WavSampleFormat::Float32);
    std::string body = "\"file\": \"audio/" + name + ".wav\", \"loadForm\": \"pcm\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}";
    if (!extra.empty()) body += ", " + extra;
    p_.Add("assets/" + name + ".json", "asset", name, body);
  }
  // `body`: segment members after asset/bars (tempo, markers, stems, entryFrame ...).
  void Segment(const std::string& name, const std::string& asset, std::uint32_t bars, const std::string& body = "") {
    std::string b = "\"asset\": \"" + asset + "\", \"bars\": " + std::to_string(bars);
    if (!body.empty()) b += ", " + body;
    p_.Add("music/" + name + ".json", "segment", name, b);
  }
  void Playlist(const std::string& name, const std::string& entries, const std::string& extra = "") {
    std::string b = "\"entries\": [" + entries + "]";
    if (!extra.empty()) b += ", " + extra;
    p_.Add("music/" + name + ".json", "playlist", name, b);
  }
  // Music event: `music` is the body of "music": {...}.
  void MusicEvent(const std::string& name, const std::string& music, const std::string& extra = "") {
    std::string b = "\"bank\": \"Main\", \"music\": {" + music + "}";
    if (!extra.empty()) b += ", " + extra;
    p_.Add("events/" + name + ".json", "event", name, b);
  }
  void Event(const std::string& name, const std::string& body) {
    p_.Add("events/" + name + ".json", "event", name, "\"bank\": \"Main\", " + body);
  }
  void Parameter(const std::string& name, const std::string& body) {
    p_.Add("params/" + name + ".json", "parameter", name, body);
  }
  // Builds; returns the bytes of Main.sndb (empty on failure, diagnostics in `diag`).
  std::vector<std::uint8_t> Build(std::string* diag = nullptr) {
    bt::Diagnostics d;
    bt::BuildOptions o = p_.Options();
    if (!p_.Build(o, &d)) {
      if (diag != nullptr) *diag = mini::Dump(d);
      return {};
    }
    return mini::ReadBytes(p_.out() / "Main.sndb");
  }
  mini::Project& raw() { return p_; }

 private:
  mini::Project p_;
};

// Transport handle owned by a music playback (D-05; snd_get_playback_transport).
inline snd_transport_handle OwnedTransport(snd_playback_handle p) {
  return snd_transport_handle{(p.value & 0xFFFFFFFF00000000ull) | ((p.value & 0xFFFFFFFFull) | SND_TRANSPORT_PLAYBACK_BIT)};
}
inline snd::SlotIndex SlotOf(snd_playback_handle p) { return static_cast<snd::SlotIndex>(p.value & 0xFFFFFFFFu); }

inline snd_when NextBar(snd_transport_handle t) {
  snd_when w{};
  w.kind = SND_WHEN_NEXT_BAR;
  w.transport = t;
  return w;
}
inline snd_when Kind(snd_transport_handle t, std::uint32_t kind, std::uint32_t count = 0) {
  snd_when w{};
  w.kind = static_cast<std::uint8_t>(kind);
  w.transport = t;
  w.count = count;
  return w;
}

// First clock >= from where |At| crosses `threshold` (normalised), or UINT64_MAX.
inline std::uint64_t FindAbove(const Rig& r, std::uint64_t from, float threshold) {
  for (std::uint64_t c = from; c < r.Rendered(); ++c) {
    if (std::fabs(r.At(c)) > threshold) return c;
  }
  return UINT64_MAX;
}

// First clock in [from, to) where the output crosses `level` (either way), or UINT64_MAX.
// The voice resampler (windowed sinc) smears steps over a few samples; a step from
// a to b crosses (a + b) / 2 within one sample of where it happens.
inline std::uint64_t Crossing(const Rig& r, std::uint64_t from, std::uint64_t to, float level) {
  const bool above = r.At(from) > level;
  for (std::uint64_t c = from; c < to && c < r.Rendered(); ++c) {
    if ((r.At(c) > level) != above) return c;
  }
  return UINT64_MAX;
}
inline std::uint64_t Diff(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }

}  // namespace mt

#endif  // SND_TESTS_INTEGRATION_MUSIC_FIXTURE_HPP
