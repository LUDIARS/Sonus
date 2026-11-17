// Independent load test (14: 128 real voices < 30 % of the block budget; 2.4).
// Own source-rate Sample kind (plays resident PCMF, looping), so every voice
// goes through core's resampler, voice filter, pan and bus mix; clips have
// different pitches. The budget is enforced in optimised builds without
// sanitizers (NDEBUG); other builds only report. Times are per block from
// System::LastBlock(), median / p99 / max over 600 blocks after a warm-up.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "indep_fixture.hpp"

using namespace indep;
using snd::HashId;

namespace {

struct Loop {
  const snd::source::SourceAsset* asset;
  std::uint32_t live;
};

class LoopSampleKind final : public snd::source::ISourceKind {
 public:
  snd::SourceKind Kind() const noexcept override { return snd::SourceKind::Sample; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  std::size_t PoolBytes(const snd::source::SourceLimits& l) const noexcept override { return sizeof(Loop) * l.maxVoices; }
  bool Init(void* pool, std::size_t, const snd::source::SourceLimits& l, const snd::source::SourceServices&) noexcept override {
    inst_ = static_cast<Loop*>(pool);
    n_ = l.maxVoices;
    for (std::uint32_t i = 0; i < n_; ++i) inst_[i] = Loop{nullptr, 0};
    return true;
  }
  void Shutdown() noexcept override {}
  void Start(const snd::source::SourceStartRequest* r, std::uint32_t n, snd::source::SourceStartResult* res) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      res[i] = snd::source::SourceStartResult{snd::kInvalidSlot, 0, 0, 0};
      if (r[i].asset == nullptr || r[i].voiceSlot >= n_) continue;
      inst_[r[i].voiceSlot] = Loop{r[i].asset, 1};
      res[i].sourceSlot = r[i].voiceSlot;
    }
  }
  void Release(const snd::SlotIndex* s, std::uint32_t n) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (s[i] < n_) inst_[s[i]].live = 0;
    }
  }
  void Control(const snd::source::SourceControlOp*, std::uint32_t) noexcept override {}
  void QueryReady(const snd::SlotIndex*, std::uint32_t n, std::uint8_t* ready) const noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) ready[i] = 1;
  }
  void RenderBatch(const snd::source::RenderBatch& b, snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < b.count; ++i) {
      const auto& it = b.items[i];
      st[i] = snd::VoiceRenderStatus{};
      if (it.sourceSlot >= n_ || inst_[it.sourceSlot].live == 0u) continue;
      const auto& a = *inst_[it.sourceSlot].asset;
      const float* d = reinterpret_cast<const float*>(a.data);
      const std::uint64_t total = a.codec.total_frames;
      std::uint64_t pos = it.playhead->frame % total;
      for (std::uint32_t f = 0; f < it.framesRequested; ++f) {
        for (std::uint32_t c = 0; c < it.channels; ++c) it.out[c][f] = d[pos];
        if (++pos == total) pos = 0;
      }
      it.playhead->frame = pos;
      st[i].framesRendered = it.framesRequested;
    }
  }
  void AdvanceVirtual(const snd::source::AdvanceItem* items, std::uint32_t n, snd::VoiceRenderStatus* st) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      st[i] = snd::VoiceRenderStatus{};
      items[i].playhead->frame += static_cast<std::uint64_t>(items[i].outputFrames * items[i].pitch);
    }
  }
  void ScheduleBackground(const snd::source::BlockInfo&) noexcept override {}
  std::uint32_t PrimeFrames(const snd::source::SourceAsset&, std::uint64_t) const noexcept override { return 0; }

 private:
  Loop* inst_ = nullptr;
  std::uint32_t n_ = 0;
};

struct Result {
  double medianUs, p99Us, maxUs;
  std::uint32_t real;
  std::uint32_t rendered;  // LastBlock().renderedVoices
  float peak;              // output peak of the last block (the voices really sound)
  std::uint32_t lod;
};

Result Measure(std::uint32_t workers, std::uint32_t voices) {
  snd_config cfg = Config();
  cfg.max_real_voices = 128;
  cfg.max_virtual_voices = 512;
  cfg.max_playbacks = 512;
  cfg.jobs.realtime_workers = workers;
  cfg.jobs.voices_per_chunk = 16;
  cfg.tuning.lod_down_blocks = 0;  // keep full quality while measuring
  LoopSampleKind kind;
  snd::source::SourceKindSet set{};
  set.kinds[static_cast<int>(snd::SourceKind::Sample)] = &kind;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  snd::core::CoreDesc d;
  d.config = &cfg;
  d.allocator = &snd::base::DefaultAllocator();
  d.sources = &set;
  d.jobs = &jobs;
  d.bankFileSystem = &fs;
  d.rtWorkerRealtimePriority = false;
  snd::core::System* sys = nullptr;
  if (snd::core::System::Create(d, &sys) != SND_OK) return Result{1e9, 1e9, 1e9, 0, 0, 0.0f, 0};
  TestBank b;
  b.buses.push_back(TBus{HashId("Sfx"), 1u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  TAsset a;
  a.id = HashId("Tone");
  a.channels = 1;
  a.rate = 44100;  // != internal rate: real resampling
  a.data.resize(44100);
  for (std::size_t i = 0; i < a.data.size(); ++i) a.data[i] = 0.01f * std::sin(6.2831853 * 441.0 * double(i) / 44100.0);
  b.assets.push_back(a);
  for (std::uint32_t i = 0; i < 16; ++i) {
    TEvent e;
    char n[8];
    std::snprintf(n, sizeof(n), "L%u", i);
    e.id = HashId(n);
    TTrack t;
    TClip c;
    c.kind = 0;
    c.target = 0;
    c.loopCount = 0;  // loop
    c.pitchCents = -600.0f + 80.0f * float(i);
    c.randPan = 1.0f;
    t.clips.push_back(c);
    e.tracks.push_back(t);
    b.events.push_back(e);
  }
  fs.AddFile("l", b.Build(HashId("L")));
  const snd_bank_handle h = sys->LoadBankAsync("l");
  std::vector<float> out(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
  tAudioThread = true;
  for (int i = 0; i < 64 && sys->GetBankState(h) != SND_BANK_STATE_LOADED; ++i) {
    sys->Update();
    sys->RenderBlock(out.data());
  }
  for (std::uint32_t v = 0; v < voices; ++v) {
    char n[8];
    std::snprintf(n, sizeof(n), "L%u", v % 16u);
    sys->Play(HashId(n), snd_emitter_handle{0}, nullptr);
  }
  for (int i = 0; i < 50; ++i) {
    sys->Update();
    sys->RenderBlock(out.data());
  }
  std::vector<double> us;
  for (int i = 0; i < 600; ++i) {
    sys->Update();
    sys->RenderBlock(out.data());
    us.push_back(double(sys->LastBlock().totalNs) / 1000.0);
  }
  const std::uint32_t rendered = sys->LastBlock().renderedVoices;
  float peak = 0.0f;
  for (float v : out) peak = std::fabs(v) > peak ? std::fabs(v) : peak;
  sys->Update();
  snd_stats st{};
  sys->GetStats(&st);
  snd::core::System::Destroy(sys);
  std::sort(us.begin(), us.end());
  return Result{us[us.size() / 2], us[us.size() * 99 / 100], us.back(), st.real_voices, rendered, peak, st.lod_level};
}

}  // namespace

SND_TEST(IndepLoad, Real128VoicesUnder30PercentAndFourTimesTheLimit) {
  const double budgetUs = 1e6 * 256.0 / 48000.0;  // one block of real time
  for (std::uint32_t workers : {0u, 2u}) {
    const Result r = Measure(workers, 128);
    std::printf("128 voices, %u RT workers: median %.1f us (%.1f %%), p99 %.1f us, max %.1f us, real %u, rendered %u, "
                "peak %.4f, lod %u\n",
                workers, r.medianUs, 100.0 * r.medianUs / budgetUs, r.p99Us, r.maxUs, r.real, r.rendered,
                static_cast<double>(r.peak), r.lod);
    SND_CHECK_EQ(r.real, 128u);
    // With RT workers a slow (debug / sanitizer) build has late chunks whose
    // voices are held and not in the render list; serial rendering has none.
    if (workers == 0u) SND_CHECK_EQ(r.rendered, 128u);
    SND_CHECK(r.peak > 0.01f);  // 128 voices of 0.01 sines: clearly audible
    SND_CHECK_EQ(r.lod, 0u);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    SND_CHECK_MSG(r.medianUs < 0.30 * budgetUs, "median block %.1f us >= 30 %% of %.1f us", r.medianUs, budgetUs);
#endif
  }
  const Result r4 = Measure(2, 512);  // 4x the real limit
  std::printf("512 requests (4x), 2 RT workers: median %.1f us (%.1f %%), max %.1f us, real %u\n", r4.medianUs,
              100.0 * r4.medianUs / budgetUs, r4.maxUs, r4.real);
  SND_CHECK_EQ(r4.real, 128u);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
  SND_CHECK_MSG(r4.medianUs < 0.30 * budgetUs, "4x: median block %.1f us", r4.medianUs);
#endif
}
