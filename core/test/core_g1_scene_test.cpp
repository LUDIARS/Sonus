// Spatial scene and budget check (D-150..157). Timings cover RenderBlock only.
#include <algorithm>
#include <array>
#include <ctime>
#include <cstring>

#include "harness.hpp"
#include "snd/test/rt_guard.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr auto kTone = HashId("moving");
constexpr std::array<snd_id, 4> kBuses{HashId("wet0"), HashId("wet1"), HashId("wet2"), HashId("wet3")};

TestBank MovingScene() {
  TestBank b;
  constexpr unsigned master = 1 + static_cast<unsigned>(kBuses.size());
  b.buses.push_back(TBus{HashId("dry"), master, 2, 0, 0, {}, {}});
  for (auto id : kBuses) b.buses.push_back(TBus{id, master, 2, 0, -12, {}, {}});
  b.buses.push_back(TBus{HashId("master"), kNone, 2, bf::kBusMaster, 0, {}, {}});
  b.assets.push_back(TAsset{HashId("loop"), 1, 48000, Sine(8192, 320, 48000), 0, 8192});
  auto e = SimpleEvent("moving", 0, 0, -42);
  e.tracks[0].clips[0].loopCount = 0;
  e.flags = bf::kEvent3D;
  e.curveSpread = 0;
  b.curves.push_back(TCurve{bf::kInputDistance, static_cast<unsigned>(bf::CurveTarget::Spread),
                            static_cast<unsigned>(bf::CurveCompose::Min), {{0, 0.8f}, {10, 0}}});
  b.events.push_back(e);
  return b;
}

void Script(unsigned workers, unsigned voices, unsigned blocks, std::vector<float>& audio) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_listeners = 2;
  cfg.max_emitters = voices + 4;
  cfg.jobs.realtime_workers = workers;
  cfg.jobs.voices_per_chunk = 8;
  cfg.listener_mode = SND_LISTENER_SPLIT;
  cfg.tuning.lod_down_blocks = 0;
  cfg.tuning.rt_spin_microseconds = 5000000;
  cfg.max_voice_channels = 2;
  CountingAllocator alloc;
  Harness h(cfg, {}, nullptr, 0, &alloc);
  SND_REQUIRE_EQ(h.created, SND_OK);
  const auto bank = h.Load("motion", MovingScene().Build(HashId("motion")));
  SND_REQUIRE_EQ(h.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  const auto allocations = alloc.allocations.load();
  snd::test::rt::Reset();
  for (unsigned l = 0; l < 2; ++l) {
    snd_listener_desc listener{{l == 0 ? -1.0f : 1.0f, 0, 0}, {0, 0, 1}, {0, 1, 0}, {}, 1, 1};
    SND_REQUIRE_EQ(h.sys->SetListener(l, &listener), SND_OK);
  }
  for (unsigned z = 0; z < kBuses.size(); ++z) {
    const auto zone = h.sys->CreateEmitter();
    const snd_vec3 half{2.0f, 1.0f, 3.0f};
    SND_REQUIRE_EQ(h.sys->SetEmitterShape(zone, SND_EMITTER_SHAPE_BOX, &half), SND_OK);
    SND_REQUIRE_EQ(h.sys->SetReverbZone(zone, kBuses[z], 0.15f * static_cast<float>(z + 1), 4), SND_OK);
  }
  std::vector<snd_emitter_handle> sources(voices);
  const snd_vec3 velocity{1, 0, -6}, forward{0, 0, -1}, extent{0.5f, 0, 0};
  for (unsigned i = 0; i < voices; ++i) {
    sources[i] = h.sys->CreateEmitter();
    SND_REQUIRE(sources[i].value != 0);
    SND_CHECK_EQ(h.sys->SetEmitterShape(sources[i], SND_EMITTER_SHAPE_LINE, &extent), SND_OK);
    SND_REQUIRE(h.sys->Play(kTone, sources[i], nullptr).value != 0);
    if (i % 32 == 31) h.Step();
  }
  h.Steps(5);
  const bool capture = voices < 128;
  std::clock_t cpu = 0;
  for (unsigned block = 0; block < blocks; ++block) {
    for (unsigned i = 0; i < voices; ++i) {
      const snd_vec3 pos{0.5f * static_cast<float>(i % 12) - 2.5f + 0.005f * static_cast<float>(block), 0,
                         5 - 0.02f * static_cast<float>(block)};
      SND_REQUIRE_EQ(h.sys->SetTransform(sources[i], &pos, &forward, &velocity), SND_OK);
    }
    h.sys->Update();
    const auto start = std::clock();
    h.sys->RenderBlock(h.out.data());
    cpu += std::clock() - start;
    SND_REQUIRE_EQ(h.sys->LastBlock().lateChunks, 0u);
    if (capture) audio.insert(audio.end(), h.out.begin(), h.out.end());
  }
  h.sys->Update();
  snd_stats stats{};
  SND_CHECK_EQ(h.sys->GetStats(&stats), SND_OK);
  SND_CHECK_EQ(stats.real_voices, voices);
  SND_CHECK_EQ(alloc.allocations.load(), allocations);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  if (!capture) {
    const double us = 1.0e6 * static_cast<double>(cpu) / static_cast<double>(CLOCKS_PER_SEC) / blocks;
    const double budget = 1.0e6 * cfg.block_size / cfg.sample_rate;
    snd::test::Note("128 moving sources, 4 zones, 2 listeners, %u RT workers: %.1f us, %.2f%%", workers, us, 100 * us / budget);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    SND_CHECK(us < budget * 0.30);
#endif
  }
}
}  // namespace

SND_TEST(CoreG1, MovingShapesAndFourZonesReplayBitExactly) {
  std::vector<float> first, second, workers;
  Script(0, 18, 18, first);
  Script(0, 18, 18, second);
  Script(2, 18, 18, workers);
  SND_REQUIRE(!first.empty());
  SND_CHECK(std::any_of(first.begin(), first.end(), [](float v) { return std::fabs(v) > 0.001f; }));
  SND_REQUIRE_EQ(first.size(), second.size());
  SND_REQUIRE_EQ(first.size(), workers.size());
  SND_CHECK_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)), 0);
  SND_CHECK_EQ(std::memcmp(first.data(), workers.data(), first.size() * sizeof(float)), 0);
}

SND_TEST(CoreG1, Split128MovingSourcesWithFourZonesWithinBudget) {
  std::vector<float> unused;
  for (unsigned workers : {0u, 2u}) Script(workers, 128, 120, unused);
}

SND_TEST(CoreG1, SplitScreenZoneGainFollowsListenerContribution) {
  auto cfg = SmallConfig();
  cfg.listener_mode = SND_LISTENER_SPLIT;
  cfg.max_listeners = 2;
  Harness h(cfg);
  SND_REQUIRE_EQ(h.created, SND_OK);
  TestBank bank;
  bank.buses.push_back(TBus{HashId("dry"), kNone, 2, bf::kBusOutput, 0, {}, {}});
  bank.buses.push_back(TBus{HashId("master"), kNone, 2, bf::kBusMaster, 0, {}, {}});
  bank.assets.push_back(TAsset{HashId("constant"), 1, 48000, Constant(4096, 0.1f), 0, 4096});
  TEvent event = SimpleEvent("moving", 0);
  event.flags = bf::kEvent3D;
  event.tracks[0].clips[0].loopCount = 0;
  event.curveSpread = 0;
  bank.curves.push_back(TCurve{bf::kInputDistance, static_cast<unsigned>(bf::CurveTarget::Spread),
                               static_cast<unsigned>(bf::CurveCompose::Min), {{0, 1}, {10, 1}}});
  bank.events.push_back(event);
  SND_REQUIRE_EQ(h.sys->GetBankState(h.Load("two seats", bank.Build(HashId("space")))), SND_BANK_STATE_LOADED);
  const auto zone = h.sys->CreateEmitter();
  const auto source = h.sys->CreateEmitter();
  const snd_vec3 atZone{-1, 0, 0};
  SND_REQUIRE_EQ(h.sys->SetTransform(zone, &atZone, nullptr, nullptr), SND_OK);
  SND_REQUIRE_EQ(h.sys->SetReverbZone(zone, HashId("master"), 1, 0.1f), SND_OK);
  snd_listener_desc inside{atZone, {0, 0, 1}, {0, 1, 0}, {}, 1, 1};
  SND_REQUIRE_EQ(h.sys->SetListener(0, &inside), SND_OK);
  SND_REQUIRE(h.sys->Play(kTone, source, nullptr).value != 0);
  h.Steps(8);
  const float reference = h.out[0] + h.out[1];
  SND_REQUIRE(reference > 0.1f);
  inside.position = {1, 0, 0};
  SND_REQUIRE_EQ(h.sys->SetListener(1, &inside), SND_OK);
  h.Steps(8);
  SND_CHECK_NEAR((h.out[0] + h.out[1]) / reference, 0.5f, 0.002f);
}

SND_TEST(CoreG1, DopplerConfigRejectsInvalidUnits) {
  auto cfg = SmallConfig();
  for (float speed : {0.0f, -1.0f, 100001.0f}) {
    cfg.speed_of_sound = speed;
    Harness h(cfg);
    SND_CHECK_EQ(h.created, SND_ERR_INVALID_ARGUMENT);
  }
  cfg.speed_of_sound = 343;
  for (float factor : {-1.0f, 16.1f}) {
    cfg.doppler_factor = factor;
    Harness h(cfg);
    SND_CHECK_EQ(h.created, SND_ERR_INVALID_ARGUMENT);
  }
}
