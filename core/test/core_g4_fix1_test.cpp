// core, stage 3 group 4, remand 1: the decisions taken on the independent tests - the fade-in
// counts played time (held while paused, running while virtual), markers while virtual,
// "never" and pause, warnings for action children over the limits, the random sequence of
// nested events, meters in the order of the bus rows, clip fade-out to 0 on the last frame.
#include <cmath>
#include <vector>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;
snd_playback_handle Play(Harness& h, const char* e) { return h.sys->Play(HashId(e), snd_emitter_handle{0}, nullptr); }
float Last(const Harness& h) { return h.At(h.TotalFrames() - 1, 0); }
}  // namespace

// D-146: the fade-in advances with played time: a pause holds it, the resume continues it.
SND_TEST(CoreG4Fix1, FadeInHoldsWhilePaused) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("F", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  const snd_playback_handle p = h.sys->PlayFade(HashId("F"), snd_emitter_handle{0}, nullptr, 0.1f);  // 4800
  h.Steps(4);  // 1024 frames played
  const float atPause = Last(h);
  h.sys->Pause(p, 0.0f);
  h.Steps(10);
  h.sys->ResumePlayback(p, 0.0f);
  h.Steps(4);
  // The resume fades in over the short fade (5 ms) and the fade-in continues from the
  // frames played (~1024 + a pause fade's frames + 1024), far below where clock time would be.
  const float after = Last(h);
  snd::test::Note("at the pause %g, 4 blocks after the resume %g (clock time would be 1.0)", double(atPause / (0.5f * kCenter)),
                  double(after / (0.5f * kCenter)));
  SND_CHECK(after > atPause);
  SND_CHECK(after < 0.5f * kCenter * (2048.0f + 512.0f) / 4800.0f);
}

// D-146: virtual, the voice keeps playing silently: its fade-in advances too.
SND_TEST(CoreG4Fix1, FadeInRunsWhileVirtual) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 1;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 0.5f), 0, 0});
  TEvent f = SimpleEvent("F", 0);
  f.priority = 100;
  TEvent hi = SimpleEvent("H", 0, 0, -200.0f);
  hi.priority = 200;
  b.events = {f, hi};
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->PlayFade(HashId("F"), snd_emitter_handle{0}, nullptr, 0.1f);  // 4800
  h.Steps(2);
  const snd_playback_handle hp = Play(h, "H");
  h.Steps(14);  // F virtual: its played time goes on (3584 + 512)
  h.sys->Stop(hp, 0.0f, nullptr);
  h.Steps(6);
  SND_CHECK_NEAR(Last(h), 0.5f * kCenter, 1e-4f);  // the fade is over (played > 4800)
}

// Markers crossed while the voice is virtual are reported, at their clock.
SND_TEST(CoreG4Fix1, MarkersWhileVirtual) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 1;
  struct Rec {
    std::vector<std::uint64_t> clocks;
  } rec;
  static const std::uint64_t kFrames[1] = {3000};
  static const snd::Id kIds[1] = {7};
  TestSampleKind sample;
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  snd::source::SourceKindSet kinds{};
  kinds.kinds[static_cast<int>(snd::SourceKind::Sample)] = &sample;
  snd::core::CoreDesc d;
  d.config = &cfg;
  d.allocator = &snd::base::DefaultAllocator();
  d.sources = &kinds;
  d.jobs = &jobs;
  d.bankFileSystem = &fs;
  d.rtWorkerRealtimePriority = false;
  d.clipMarkers = [](void*, snd::Id asset) noexcept {
    return asset == HashId("m") ? snd::core::CoreDesc::ClipMarkers{kFrames, kIds, 1u}
                                : snd::core::CoreDesc::ClipMarkers{nullptr, nullptr, 0u};
  };
  snd::core::System* sys = nullptr;
  SND_REQUIRE_EQ(snd::core::System::Create(d, &sys), SND_OK);
  snd_callbacks cb{};
  cb.user = &rec;
  cb.on_marker = [](void* u, snd_playback_handle, snd_id, const snd_timing* t) {
    static_cast<Rec*>(u)->clocks.push_back(t->clock_sample);
  };
  sys->SetCallbacks(&cb);
  std::vector<float> out(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
  auto step = [&] {
    sys->Update();
    sys->RenderBlock(out.data());
  };
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("m"), 1, 48000, Constant(20000, 0.1f), 0, 0});
  b.assets.push_back(TAsset{HashId("x"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent m = SimpleEvent("M", 0);
  m.priority = 100;
  TEvent hi = SimpleEvent("H", 1);
  hi.priority = 200;
  b.events = {m, hi};
  fs.AddFile("b", b.Build(HashId("B")));
  const snd_bank_handle bank = sys->LoadBankAsync("b");
  for (int i = 0; i < 64 && sys->GetBankState(bank) != SND_BANK_STATE_LOADED; ++i) step();
  step();
  sys->Update();
  snd_stats st{};
  sys->GetStats(&st);
  const std::uint64_t start = st.clock_sample;
  sys->Play(HashId("M"), snd_emitter_handle{0}, nullptr);
  step();
  step();
  sys->Play(HashId("H"), snd_emitter_handle{0}, nullptr);  // M virtual from frame 512 on
  for (int i = 0; i < 20; ++i) step();
  sys->Update();
  SND_REQUIRE_EQ(rec.clocks.size(), std::size_t{1});
  const long long dlt = static_cast<long long>(rec.clocks[0]) - static_cast<long long>(start + 3000);
  snd::test::Note("marker at frame 3000 crossed while virtual: clock %+lld", dlt);
  SND_CHECK(dlt >= -1 && dlt <= 1);
  snd::core::System::Destroy(sys);
}

// "Never" and pause: a paused voice is not ended, whatever the order; once resumed, a voice
// that cannot be real ends then.
SND_TEST(CoreG4Fix1, NeverIsNotEndedWhilePaused) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 1;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent v = SimpleEvent("V", 0);
  v.priority = 100;
  v.virtualReturn = 2;
  TEvent hi = SimpleEvent("H", 0);
  hi.priority = 200;
  b.events = {v, hi};
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle vp = Play(h, "V");
  h.Steps(2);
  h.sys->Pause(vp, 0.0f);  // the pause and the steal in the same block
  const snd_playback_handle hp = Play(h, "H");
  h.Steps(8);
  SND_CHECK_EQ(h.ends.CountOf(vp), 0u);
  h.sys->ResumePlayback(vp, 0.0f);  // H still plays: V cannot be real -> ends now
  h.Steps(4);
  SND_CHECK(h.ends.Has(vp, SND_END_STOLEN, SND_END_DETAIL_VOICE_LIMIT));
  SND_CHECK_EQ(h.ends.CountOf(hp), 0u);
}

// Action children over the limits: a LIMIT_REACHED warning (object = the parent).
SND_TEST(CoreG4Fix1, ActionChildrenOverTheLimitWarn) {
  snd::core::CoreLimits lim;
  lim.maxActionChildren = 4;
  snd_config cfg = SmallConfig();
  cfg.max_playbacks = 1024;
  cfg.max_virtual_voices = 512;
  Harness h(cfg, lim);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 0.0f), 0, 0});
  TEvent p = SimpleEvent("P", 0);
  for (int i = 0; i < 8; ++i) p.tracks[0].actions.push_back(TAction{0.0f, 0u, HashId("C"), 0.0f});      // first block
  for (int i = 0; i < 300; ++i) p.tracks[0].actions.push_back(TAction{20.0f, 0u, HashId("C"), 0.0f});  // later (queue 256)
  b.events = {p, SimpleEvent("C", 0)};
  h.Load("b", b.Build(HashId("B")));
  h.ends.errors.clear();
  Play(h, "P");
  h.Steps(8);
  std::uint32_t warnings = 0;
  for (snd_result r : h.ends.errors) warnings += r == SND_ERR_LIMIT_REACHED ? 1u : 0u;
  snd::test::Note("warnings %u", warnings);
  SND_CHECK(warnings >= 2u);  // the first block's limit and the later queue's
}

// Nested events draw from their own random sequence (the playback's seed and their id): C
// sounds the same whether another nested event (A, silent, with random ranges) comes first.
SND_TEST(CoreG4Fix1, NestedEventsHaveTheirOwnRandomSequence) {
  auto run = [](bool withA) {
    Harness h;
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(4800, 0.1f), 0, 0});
    b.assets.push_back(TAsset{HashId("zero"), 1, 48000, Constant(4800, 0.0f), 0, 0});
    TEvent p = SimpleEvent("P", 0);
    p.tracks[0].clips.clear();
    TClip na;
    na.kind = 1;
    na.target = 1;
    TClip nc = na;
    nc.target = 2;
    if (withA) p.tracks[0].clips.push_back(na);
    p.tracks[0].clips.push_back(nc);
    TEvent a = SimpleEvent("A", 1);
    a.tracks[0].clips[0].randVolumeDb = 6.0f;
    a.tracks[0].clips[0].randPan = 1.0f;
    TEvent c = SimpleEvent("C", 0);
    c.tracks[0].clips[0].randVolumeDb = 6.0f;
    c.tracks[0].clips[0].randPan = 1.0f;
    b.events = {p, a, c};
    h.Load("b", b.Build(HashId("B")));
    h.Clear();
    h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
    h.Steps(2);
    return std::pair<float, float>{h.At(300, 0), h.At(300, 1)};
  };
  const auto x = run(true), y = run(false);
  snd::test::Note("C with A first: %g / %g, alone: %g / %g", double(x.first), double(x.second), double(y.first), double(y.second));
  SND_CHECK_EQ(x.first, y.first);
  SND_CHECK_EQ(x.second, y.second);
}

// The meters go to kBusMeter buses in the order of the bus rows, sounding or not.
SND_TEST(CoreG4Fix1, MetersInTheOrderOfTheBusRows) {
  snd::core::CoreLimits lim;
  lim.maxMeteredBuses = 1;
  Harness h(SmallConfig(), lim);
  TestBank b;
  b.buses.push_back(TBus{HashId("M1"), 2u, 0, bf::kBusMeter, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("M2"), 2u, 0, bf::kBusMeter, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Sine(400000, 1000.0, 48000.0, 0.3f), 0, 0});
  b.events.push_back(SimpleEvent("On2", 0, 1));
  h.Load("b", b.Build(HashId("B")));
  Play(h, "On2");
  h.Steps(20);
  h.sys->Update();
  snd_meter m1{}, m2{};
  h.sys->GetMeter(HashId("M1"), &m1);
  h.sys->GetMeter(HashId("M2"), &m2);
  SND_CHECK(!std::isnan(m1.loudness_short));  // the first row has the meter (silent)
  SND_CHECK(std::isnan(m2.loudness_short));
}

// CLIP.fadeOutMs: the last F frames go from 1 to 0, 0 on the last frame (ratio 1: exact).
SND_TEST(CoreG4Fix1, ClipFadeOutReachesZeroOnTheLastFrame) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(4800, 1.0f), 0, 0});
  TEvent e = SimpleEvent("F", 0);
  e.tracks[0].clips[0].fadeOutMs = 50.0f;  // frames 2400 .. 4799
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  Play(h, "F");
  h.Steps(24);
  SND_CHECK_NEAR(h.At(2399, 0) / kCenter, 1.0f, 1e-4f);
  SND_CHECK_NEAR(h.At(2400, 0) / kCenter, 1.0f, 1e-4f);
  SND_CHECK_NEAR(h.At(3600, 0) / kCenter, 1199.0f / 2399.0f, 1e-4f);
  SND_CHECK_NEAR(h.At(4799, 0) / kCenter, 0.0f, 1e-4f);
}
