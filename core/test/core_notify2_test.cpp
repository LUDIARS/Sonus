// core, stage 3 group 4: notifications during playback (2.1) - every end reason exactly
// once where it happens, and markers of effect clips (CoreDesc::clipMarkers until the bank
// has a column; markers.hpp).
#include <cmath>
#include <vector>

#include "harness.hpp"
#include "../src/markers.hpp"

using namespace coretest;
using snd::HashId;

namespace {
snd_playback_handle Play(Harness& h, const char* e) { return h.sys->Play(HashId(e), snd_emitter_handle{0}, nullptr); }
}  // namespace

// Each reason, each exactly once: COMPLETED (the source ends), STOPPED (Stop), REJECTED
// (event limit), NOT_LOADED (unknown event), DISCARDED (the bank goes), STOLEN (a "never"
// return voice went virtual, D-140).
SND_TEST(CoreNotify2, EveryEndReasonExactlyOnce) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 2;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("short"), 1, 48000, Constant(1000, 0.1f), 0, 0});
  b.assets.push_back(TAsset{HashId("long"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent shortE = SimpleEvent("Short", 0);
  TEvent longE = SimpleEvent("Long", 1);
  TEvent one = SimpleEvent("One", 1);
  one.maxInstances = 1;
  TEvent never = SimpleEvent("Never", 1);
  never.priority = 10;
  never.virtualReturn = 2;
  TEvent hi = SimpleEvent("Hi", 1);
  hi.priority = 250;
  b.events = {shortE, longE, one, never, hi};
  const snd_bank_handle bank = h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle completed = Play(h, "Short");
  const snd_playback_handle stopped = Play(h, "Long");
  h.Steps(2);
  const snd_playback_handle never1 = Play(h, "Never");  // real (2 real voices: Long, Never)
  h.Steps(2);
  h.sys->Stop(stopped, 0.0f, nullptr);
  const snd_playback_handle hi1 = Play(h, "Hi");  // pushes Never out once Long is gone
  const snd_playback_handle hi2 = Play(h, "Hi");
  const snd_playback_handle one1 = Play(h, "One");
  const snd_playback_handle one2 = Play(h, "One");
  const snd_playback_handle unknown = h.sys->Play(HashId("Nope"), snd_emitter_handle{0}, nullptr);
  h.Steps(10);
  SND_CHECK(h.ends.Has(completed, SND_END_COMPLETED, SND_END_DETAIL_NONE));
  SND_CHECK(h.ends.Has(stopped, SND_END_STOPPED, SND_END_DETAIL_NONE));
  // Two plays of One in one block: one of them is refused (which one is not specified).
  const bool firstWins = h.ends.Has(one2, SND_END_REJECTED, SND_END_DETAIL_EVENT_LIMIT);
  SND_CHECK(firstWins || h.ends.Has(one1, SND_END_REJECTED, SND_END_DETAIL_EVENT_LIMIT));
  const snd_playback_handle oneLive = firstWins ? one1 : one2;
  SND_CHECK(h.ends.Has(unknown, SND_END_NOT_LOADED, SND_END_DETAIL_NONE));
  SND_CHECK(h.ends.Has(never1, SND_END_STOLEN, SND_END_DETAIL_VOICE_LIMIT));
  h.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD);
  h.Steps(10);
  SND_CHECK(h.ends.Has(hi1, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  SND_CHECK(h.ends.Has(hi2, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  SND_CHECK(h.ends.Has(oneLive, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  h.Steps(10);
  for (snd_playback_handle p : {completed, stopped, never1, hi1, hi2, one1, one2, unknown}) {
    SND_CHECK_EQ(h.ends.CountOf(p), 1u);
  }
  SND_CHECK_EQ(h.ends.list.size(), std::size_t{8});
}

// REJECTED / COMMAND_OVERFLOW: a play the arena could not hold ends at the next Update,
// once (record time, 2.1).
SND_TEST(CoreNotify2, CommandOverflowEndsOnce) {
  snd_config cfg = SmallConfig();
  cfg.commands.arena_bytes = 1024;
  cfg.commands.reserve_bytes = 256;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(1000, 0.1f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  std::vector<snd_playback_handle> plays;
  for (int i = 0; i < 40; ++i) plays.push_back(Play(h, "E"));
  h.Steps(10);
  std::uint32_t overflow = 0, completed = 0;
  for (snd_playback_handle p : plays) {
    SND_CHECK_EQ(h.ends.CountOf(p), 1u);
    overflow += h.ends.Has(p, SND_END_REJECTED, SND_END_DETAIL_COMMAND_OVERFLOW) ? 1u : 0u;
    completed += h.ends.Has(p, SND_END_COMPLETED) ? 1u : 0u;
  }
  SND_CHECK(overflow > 0u);
  SND_CHECK(completed > 0u);
  SND_CHECK_EQ(overflow + completed, 40u);
}

// A nested event's voices and an action's child playbacks: one end, for the playback the
// game holds (the children of actions are one-shots: no callback).
SND_TEST(CoreNotify2, NestedAndActionPlaybacksEndOnceForTheParent) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(3000, 0.1f), 0, 0});
  TEvent p = SimpleEvent("P", 0);
  TClip nested;
  nested.kind = 1;
  nested.target = 1;
  p.tracks[0].clips.push_back(nested);
  p.tracks[0].actions.push_back(TAction{0.0f, 0u, HashId("C"), 0.0f});
  p.tracks[0].actions.push_back(TAction{20.0f, 0u, HashId("C"), 0.0f});
  b.events = {p, SimpleEvent("C", 0)};
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle ph = Play(h, "P");
  h.Steps(30);
  SND_CHECK(h.ends.Has(ph, SND_END_COMPLETED));
  SND_CHECK_EQ(h.ends.list.size(), std::size_t{1});
}

// --- markers ------------------------------------------------------------------------------
SND_TEST(CoreNotify2, MarkerCrossingsOfAPlainAndALoopingClip) {
  const std::uint64_t frames[3] = {100, 600, 50};
  std::vector<std::pair<std::uint32_t, double>> got;
  auto rec = [&](std::uint32_t i, double p) { got.emplace_back(i, p); };
  // Plain clip from frame 0: [0, 700) crosses 50, 100, 600 in order.
  snd::core::ForEachMarkerCrossing(frames, 3, 0, 0, 0, 0.0, 700.0, rec);
  SND_REQUIRE_EQ(got.size(), std::size_t{3});
  SND_CHECK(got[0].first == 2 && got[0].second == 50.0);
  SND_CHECK(got[1].first == 0 && got[1].second == 100.0);
  SND_CHECK(got[2].first == 1 && got[2].second == 600.0);
  // [from, to): from included, to excluded.
  got.clear();
  snd::core::ForEachMarkerCrossing(frames, 3, 0, 0, 0, 100.0, 600.0, rec);
  SND_REQUIRE_EQ(got.size(), std::size_t{1});
  SND_CHECK(got[0].first == 0 && got[0].second == 100.0);
  // A start offset of 80: frame 50 is never read; 100 at played 20.
  got.clear();
  snd::core::ForEachMarkerCrossing(frames, 3, 80, 0, 0, 0.0, 1000.0, rec);
  SND_REQUIRE_EQ(got.size(), std::size_t{2});
  SND_CHECK(got[0].second == 20.0 && got[1].second == 520.0);
  // Loop [0, 400): 50 and 100 every pass (played 50, 100, 450, 500, 850, 900), 600 never.
  got.clear();
  snd::core::ForEachMarkerCrossing(frames, 3, 0, 0, 400, 0.0, 1000.0, rec);
  SND_REQUIRE_EQ(got.size(), std::size_t{6});
  const double expect[6] = {50, 100, 450, 500, 850, 900};
  for (int i = 0; i < 6; ++i) SND_CHECK_EQ(got[static_cast<std::size_t>(i)].second, expect[i]);
  // Loop [200, 700) from 0: pass 0 has 50, 100, 600; then 600 at 1100, 1600 ...
  got.clear();
  snd::core::ForEachMarkerCrossing(frames, 3, 0, 200, 700, 650.0, 1700.0, rec);
  SND_REQUIRE_EQ(got.size(), std::size_t{2});
  SND_CHECK(got[0].second == 1100.0 && got[1].second == 1600.0);
}

namespace {
// A system with markers for the asset "m" (CoreDesc::clipMarkers) and on_marker recorded.
struct MarkerRig {
  struct Marker {
    std::uint64_t playback;
    snd::Id id;
    std::uint64_t clock;
  };
  static constexpr std::uint64_t kFrames[2] = {300, 1000};
  static constexpr snd::Id kIds[2] = {11, 22};
  snd::mock::MemoryFileSystem fs;
  snd::mock::DeterministicJobSystem jobs;
  TestSampleKind sample;
  snd::source::SourceKindSet kinds{};
  snd::core::System* sys = nullptr;
  std::vector<Marker> markers;
  std::vector<float> out;
  std::uint64_t firstClock = 0;
  explicit MarkerRig(const snd_config& cfg) {
    kinds.kinds[static_cast<int>(snd::SourceKind::Sample)] = &sample;
    snd::core::CoreDesc d;
    d.config = &cfg;
    d.allocator = &snd::base::DefaultAllocator();
    d.sources = &kinds;
    d.jobs = &jobs;
    d.bankFileSystem = &fs;
    d.rtWorkerRealtimePriority = false;
    d.clipMarkers = [](void*, snd::Id asset) noexcept {
      return asset == HashId("m") ? snd::core::CoreDesc::ClipMarkers{kFrames, kIds, 2u}
                                  : snd::core::CoreDesc::ClipMarkers{nullptr, nullptr, 0u};
    };
    snd::core::System::Create(d, &sys);
    snd_callbacks cb{};
    cb.user = this;
    cb.on_marker = [](void* u, snd_playback_handle p, snd_id id, const snd_timing* t) {
      static_cast<MarkerRig*>(u)->markers.push_back(Marker{p.value, id, t->clock_sample});
    };
    sys->SetCallbacks(&cb);
    out.resize(std::size_t{sys->BlockFrames()} * sys->OutputChannels());
  }
  ~MarkerRig() { snd::core::System::Destroy(sys); }
  void Step() {
    sys->Update();
    sys->RenderBlock(out.data());
  }
};
}  // namespace

// End to end: on_marker for each crossing, with the playback and the clock of the crossing
// (the resampler's look-ahead makes it up to 16 frames early for source-rate voices).
SND_TEST(CoreNotify2, ClipMarkersReachOnMarker) {
  MarkerRig r(SmallConfig());
  SND_REQUIRE(r.sys != nullptr);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("m"), 1, 48000, Constant(1200, 0.1f), 0, 0});
  b.assets.push_back(TAsset{HashId("x"), 1, 48000, Constant(1200, 0.1f), 0, 0});
  TEvent loop = SimpleEvent("Loop", 0);
  loop.tracks[0].clips[0].loopCount = 2;  // 2400 frames: the markers twice
  b.events = {loop, SimpleEvent("Other", 1)};
  r.fs.AddFile("b", b.Build(HashId("B")));
  const snd_bank_handle bank = r.sys->LoadBankAsync("b");
  for (int i = 0; i < 64 && r.sys->GetBankState(bank) != SND_BANK_STATE_LOADED; ++i) r.Step();
  r.Step();
  r.sys->Update();
  snd_stats st{};
  r.sys->GetStats(&st);
  const std::uint64_t start = st.clock_sample;
  const snd_playback_handle p = r.sys->Play(HashId("Loop"), snd_emitter_handle{0}, nullptr);
  r.sys->Play(HashId("Other"), snd_emitter_handle{0}, nullptr);  // no markers
  for (int i = 0; i < 16; ++i) r.Step();
  r.sys->Update();
  SND_REQUIRE_EQ(r.markers.size(), std::size_t{4});
  const std::uint64_t at[4] = {300, 1000, 1500, 2200};
  const snd::Id ids[4] = {11, 22, 11, 22};
  for (std::size_t i = 0; i < 4; ++i) {
    SND_CHECK_EQ(r.markers[i].playback, p.value);
    SND_CHECK_EQ(r.markers[i].id, ids[i]);
    const std::int64_t d = static_cast<std::int64_t>(r.markers[i].clock) - static_cast<std::int64_t>(start + at[i]);
    SND_CHECK_MSG(d <= 0 && d >= -17, "marker %u at %+lld frames", static_cast<unsigned>(i), static_cast<long long>(d));
  }
}
