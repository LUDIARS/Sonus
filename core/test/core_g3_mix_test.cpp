// Stage 3 group 3. Measure relative changes to a steady mono source.
#include <cmath>
#include <ctime>
#include <cstring>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/test/rt_guard.hpp"

using namespace coretest;
using snd::HashId;

namespace {
TestBank SignalBank() {
  auto b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(48000, 0.1f), 0, 48000});
  b.events.push_back(SimpleEvent("E", 0));
  b.events[0].tracks[0].clips[0].loopCount = 0u;
  return b;
}
float Level(Harness& h) {
  h.Steps(4);
  return h.out.back();
}
}

SND_TEST(CoreG3, ForbiddenScopeKeepsAllowedValue) {
  auto b = SignalBank();
  b.params.push_back(TParam{HashId("P"), 0, -24, 0, 0, 0, 1});
  b.curves.push_back(TCurve{HashId("P"), 0, 0, {{-24,-24}, {-6,-6}, {0,0}}});
  b.events[0].curves = {0};
  Harness h;
  h.Load("b", b.Build(HashId("B")));
  const auto em = h.sys->CreateEmitter();
  const auto p = h.sys->Play(HashId("E"), em, nullptr);
  const auto base = Level(h);
  SND_CHECK(base > 0.0f);
  h.sys->SetParamGlobal(HashId("P"), -6.0f);
  h.sys->SetParamEmitter(em, HashId("P"), -18.0f);
  h.sys->SetParamPlayback(p, HashId("P"), -24.0f);
  SND_CHECK_NEAR(Level(h), base * std::pow(10.0f, -0.3f), 0.00001f);
}

SND_TEST(CoreG3, SoloSubtreeAndFocusRestoreIndependentMute) {
  auto cfg = SmallConfig();
  cfg.focus_mode = SND_FOCUS_PER_CATEGORY;
  Harness h(cfg);
  auto b = SignalBank();
  b.categories = {{HashId("child"),1,0,0}, {HashId("parent"),kNone,bf::kCategoryMuteOnFocusLoss,0},
                  {HashId("other"),kNone,0,0}};
  b.events[0].category = 0;
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("E"), {}, nullptr);
  const float base = Level(h);
  SND_CHECK(base > 0.0f);
  h.sys->SetCategorySolo(HashId("other"), true);
  SND_CHECK_EQ(Level(h), 0.0f);
  h.sys->SetCategorySolo(HashId("parent"), true);
  SND_CHECK_NEAR(Level(h), base, 1e-6f);
  h.sys->SetFocusLost(true);
  SND_CHECK_EQ(Level(h), 0.0f);
  h.sys->MuteCategory(HashId("child"), true);
  h.sys->SetFocusLost(false);
  SND_CHECK_EQ(Level(h), 0.0f);
  h.sys->MuteCategory(HashId("child"), false);
  SND_CHECK_NEAR(Level(h), base, 1e-6f);
}

SND_TEST(CoreG3, LongSnapshotActionAtHighSampleRateKeepsFading) {
  auto bank = SignalBank();
  const auto quiet = HashId("Quiet");
  bank.snapshots.push_back({quiet, {{0, 0, 0, -24}}});
  bank.events[0].tracks[0].actions = {{0, static_cast<unsigned>(bf::ActionKind::ApplySnapshot), quiet, 1}};
  auto cfg = SmallConfig();
  cfg.sample_rate = 131072;
  const auto bytes = bank.Build(HashId("Bank"));
  const auto levelAfterAction = [&](float seconds) {
    Harness h(cfg);
    h.Load("b", bytes);
    h.sys->SetSnapshotActionTransition(quiet, seconds);
    h.sys->Play(HashId("E"), {}, nullptr);
    return Level(h);
  };
  const float immediate = levelAfterAction(0);
  const float longFade = levelAfterAction(32768);  // 2^32 frames before saturation
  SND_CHECK(immediate > 0.0f);
  SND_CHECK_NEAR(longFade, immediate * std::pow(10.0f, 24.0f / 20.0f), 1e-5f);
}

SND_TEST(CoreG3, SnapshotSendOffsetsStackAndRelease) {
  auto b = SignalBank();
  b.buses[0].parent = kNone;
  b.buses[0].flags = bf::kBusOutput;
  b.buses[0].sends = {{1u, -6.0f, 1u}};
  b.snapshots = {{HashId("A"),{{2,0,0,12}}}, {HashId("B"),{{2,0,0,-12}}}};
  Harness h;
  h.Load("b", b.Build(HashId("SendBank")));
  h.sys->Play(HashId("E"), {}, nullptr);
  const float base = Level(h);
  SND_CHECK(base > 0.0f);
  h.sys->ApplySnapshot(HashId("A"), 0.5f, 0);
  SND_CHECK_NEAR(Level(h), base * 1.9952623f, 1e-5f);
  h.sys->ApplySnapshot(HashId("B"), 0.5f, 0);
  SND_CHECK_NEAR(Level(h), base, 1e-6f);
  h.sys->ApplySnapshot(HashId("A"), 0.0f, 0);
  SND_CHECK_NEAR(Level(h), base / 1.9952623f, 1e-5f);
}

namespace {
std::vector<float> RunGroupMix(std::uint32_t workers, std::uint32_t voices) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_real_voices = 128;
  cfg.max_virtual_voices = cfg.max_playbacks = 160;
  cfg.jobs.realtime_workers = workers;
  cfg.tuning.rt_spin_microseconds = 200000;
  cfg.tuning.lod_down_blocks = 0;
  cfg.focus_mode = SND_FOCUS_PER_CATEGORY;
  CountingAllocator alloc;
  Harness h(cfg, {}, nullptr, 0, &alloc);
  auto b = SignalBank();
  b.buses = {{HashId("Signal"), 2, 2, 0, -30, {0}, {{2, -14, 1}}},
             {HashId("Key"), kNone, 2, bf::kBusOutput, 0, {}, {}},
             {HashId("Master"), kNone, 2, bf::kBusMaster, 0, {}, {}}};
  b.effects = {{snd::dsp::kEffectLowpass, {1800, 0.707f}}};
  b.snapshots = {{HashId("Mix"), {{0,0,0,-2}, {1,0,0,200}, {2,0,0,-6}}}};
  b.ducks = {{0,1,0,8,4,10,-36}, {1,0,0,3,2,8,0}};
  b.categories = {{HashId("Cue"),kNone,0,0}};
  b.params = {{HashId("Vol"),0,-12,0,-3,4,7}};
  b.curves = {{HashId("Vol"),0,0,{{-12,-15},{-6,-7},{0,0}}}};
  b.events[0].curves = {0};
  b.events[0].category = 0;
  auto key = b.events[0];
  key.id = HashId("K");
  key.bus = 1;
  b.events.push_back(key);
  h.Load("mix", b.Build(HashId("Bank")));
  h.sys->SetCategorySolo(HashId("Cue"), true);
  const auto em = h.sys->CreateEmitter();
  h.sys->SetParamGlobal(HashId("Vol"), -4);
  h.sys->SetParamEmitter(em, HashId("Vol"), -2);
  for (std::uint32_t i = 0; i < voices; ++i) {
    const auto p = h.sys->Play(i ? HashId("E") : HashId("K"), em, nullptr);
    if (i % 3u == 0u) h.sys->SetParamPlayback(p, HashId("Vol"), -6);
    if (i % 32u == 31u) h.Step();
  }
  h.Steps(8);
  snd::test::rt::Reset();
  const auto allocations = alloc.allocations.load();
  std::clock_t ticks = 0;
  std::vector<float> captured;
  constexpr unsigned blocks = 48;
  for (unsigned i = 0; i < blocks; ++i) {
    if (i % 8u == 0u) h.sys->ApplySnapshot(HashId("Mix"), (i % 16u == 0u) ? 1.0f : 0.5f, 0.02f);
    h.sys->Update();
    const auto before = std::clock();
    h.sys->RenderBlock(h.out.data());
    ticks += std::clock() - before;
    captured.insert(captured.end(), h.out.begin(), h.out.end());
  }
  h.sys->Update();
  snd_stats stats{};
  h.sys->GetStats(&stats);
  SND_CHECK_EQ(stats.real_voices, voices);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  SND_CHECK_EQ(alloc.allocations.load(), allocations);
  if (voices == 128) {
    const double us = 1e6 * static_cast<double>(ticks) / (blocks * static_cast<double>(CLOCKS_PER_SEC));
    const double budget = 1e6 * h.Frames() / cfg.sample_rate;
    snd::test::Note("128 voices, %u RT workers, group 3 CPU %.1f us (%.1f %%)", workers, us, 100 * us / budget);
#if defined(NDEBUG) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    SND_CHECK(us < 0.3 * budget);
#endif
  }
  return captured;
}
}

SND_TEST(CoreG3, WeightedMixReplaysBitExactly) {
  const auto a = RunGroupMix(0, 20);
  const auto b = RunGroupMix(0, 20);
  const auto c = RunGroupMix(2, 20);
  SND_CHECK(a.size() == b.size() && a.size() == c.size());
  SND_CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
  SND_CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
}

SND_TEST(CoreG3, WeightedMixBudgetFor128RealVoices) {
  RunGroupMix(0, 128);
  RunGroupMix(2, 128);
}
