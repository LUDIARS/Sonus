// core: banks (registration, A1-16 unification, unload and epoch), command
// queues (rejection, must-deliver fallback, other registered threads),
// notifications, determinism, memory after initialisation, RT safety.
#include <cmath>
#include <cstring>
#include <thread>

#include "harness.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/rt_guard.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;
float Mean(const Harness& h, std::size_t from, std::size_t to, std::uint32_t c) {
  double s = 0;
  for (std::size_t f = from; f < to; ++f) s += h.At(f, c);
  return static_cast<float>(s / double(to - from));
}
// Bank with a shared bus "Music" (-6 dB in this bank) and one event on it.
std::vector<std::uint8_t> SharedBusBank(const char* bank, const char* event, float level, float busDb) {
  TestBank b;
  b.buses.push_back(TBus{HashId("Music"), 1u, 0, 0, busDb, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.assets.push_back(TAsset{HashId(event), 1, 48000, Constant(480000, level), 0, 0});
  b.events.push_back(SimpleEvent(event, 0, 0));
  return b.Build(HashId(bank));
}
}  // namespace

SND_TEST(CoreBanks, SharedBusIsOneRowAcrossBanks) {
  Harness h;
  const snd_bank_handle a = h.Load("a", SharedBusBank("A", "EA", 0.1f, -6.0f));
  const snd_bank_handle b = h.Load("b", SharedBusBank("B", "EB", 0.2f, -6.0f));
  SND_REQUIRE_EQ(h.sys->GetBankState(a), SND_BANK_STATE_LOADED);
  SND_REQUIRE_EQ(h.sys->GetBankState(b), SND_BANK_STATE_LOADED);
  h.sys->Play(HashId("EA"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("EB"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(4);
  const float g = std::pow(10.0f, -6.0f / 20.0f);
  SND_CHECK_NEAR(Mean(h, 3 * 256, 4 * 256, 0), kCenter * 0.3f * g, 1e-4f);
  // One volume change by id reaches both banks' voices (one bus, A1-16).
  h.sys->SetBusVolume(HashId("Music"), 0.5f, 0.0f);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 2 * 256, 3 * 256, 0), kCenter * 0.3f * g * 0.5f, 1e-4f);
  h.sys->Update();
  snd_meter m{};
  SND_CHECK_EQ(h.sys->GetMeter(HashId("Music"), &m), SND_OK);
  // Unloading A keeps the bus for B (definition moves to B), with the volume set.
  SND_CHECK_EQ(h.sys->UnloadBank(a, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  h.Steps(30);
  SND_CHECK_EQ(h.sys->GetBankState(a), SND_BANK_STATE_UNLOADED);
  h.Clear();
  h.Steps(2);
  SND_CHECK_NEAR(Mean(h, 256, 2 * 256, 0), kCenter * 0.2f * g * 0.5f, 1e-4f);
}

SND_TEST(CoreBanks, StopAndUnloadDiscardsAndIfUnusedRefuses) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(480000, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  const snd_bank_handle bank = h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle p = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  SND_CHECK_EQ(h.sys->UnloadBank(bank, SND_UNLOAD_IF_UNUSED), SND_OK);  // recorded; refused on the audio side
  h.Steps(4);
  SND_CHECK_EQ(h.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  SND_CHECK(!h.ends.errors.empty() && h.ends.errors.back() == SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(h.ends.CountOf(p), 0u);
  SND_CHECK_EQ(h.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  h.Clear();
  h.Steps(10);
  SND_CHECK(h.ends.Has(p, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  SND_CHECK_EQ(h.sys->GetBankState(bank), SND_BANK_STATE_UNLOADED);
  // The discard fades (D-15) instead of cutting: the first block still decays.
  SND_CHECK(h.At(0, 0) > 0.3f);
  SND_CHECK_NEAR(Mean(h, 5 * 256, 10 * 256, 0), 0.0f, 1e-7f);
  // Reload: the same events play again.
  const snd_bank_handle again = h.Load("b", b.Build(HashId("B")));
  SND_CHECK_EQ(h.sys->GetBankState(again), SND_BANK_STATE_LOADED);
  const snd_playback_handle p2 = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 2 * 256, 3 * 256, 0), 0.5f * kCenter, 1e-4f);
  SND_CHECK_EQ(h.ends.CountOf(p2), 0u);
}

SND_TEST(CoreCommands, RejectedPlayEndsWithCommandOverflowAndStopsStillArrive) {
  snd_config cfg = SmallConfig();
  cfg.commands.arena_bytes = 512;   // a handful of commands
  cfg.commands.reserve_bytes = 64;  // one or two must-deliver commands
  cfg.max_playbacks = 256;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(480000, 0.01f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  std::vector<snd_playback_handle> ps;
  for (int i = 0; i < 20; ++i) ps.push_back(h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr));
  h.Steps(3);
  std::uint32_t rejected = 0, alive = 0;
  for (const snd_playback_handle p : ps) {
    if (h.ends.Has(p, SND_END_REJECTED, SND_END_DETAIL_COMMAND_OVERFLOW)) ++rejected;
    if (h.ends.CountOf(p) == 0u) ++alive;
  }
  SND_CHECK(rejected > 0u);
  SND_CHECK_EQ(rejected + alive, 20u);
  // Many stops at once: beyond the reserve they are coalesced (D-11), none is lost.
  for (int k = 0; k < 4; ++k) {
    for (const snd_playback_handle p : ps) h.sys->Stop(p, 0.0f, nullptr);
  }
  h.Steps(6);
  for (const snd_playback_handle p : ps) SND_CHECK_EQ(h.ends.CountOf(p), 1u);
  h.sys->Update();
  snd_stats st{};
  h.sys->GetStats(&st);
  SND_CHECK(st.rejected_commands >= rejected);
  SND_CHECK_EQ(st.reserve_overflows, 0u);  // coalesced stops are delivered, nothing was lost (D-11)
}

SND_TEST(CoreCommands, OneShotsHaveNoEndCallbackAndOtherThreadsRecord) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(600, 0.25f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  SND_CHECK_EQ(h.sys->PlayOneShot(HashId("E"), snd_emitter_handle{0}, nullptr), SND_OK);
  const snd_vec3 pos{1, 0, 0};
  SND_CHECK_EQ(h.sys->PlayAt(HashId("E"), &pos, nullptr), SND_OK);
  h.Steps(10);
  SND_CHECK(h.ends.list.empty());
  // A second registered thread records and flushes its own arena.
  snd_playback_handle fromThread{0};
  std::thread t([&] {
    SND_CHECK_EQ(h.sys->RegisterThread(), SND_OK);
    fromThread = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
    SND_CHECK_EQ(h.sys->Flush(), SND_OK);
    SND_CHECK_EQ(h.sys->UnregisterThread(), SND_OK);
  });
  t.join();
  SND_CHECK(fromThread.value != 0u);
  h.Steps(8);
  SND_CHECK(h.ends.Has(fromThread, SND_END_COMPLETED));
  // An unregistered thread records nothing.
  std::thread u([&] { SND_CHECK_EQ(h.sys->Stop(fromThread, 0.0f, nullptr), SND_ERR_THREAD_NOT_REGISTERED); });
  u.join();
  // Stale handles are refused at record time (D-29).
  SND_CHECK_EQ(h.sys->Stop(fromThread, 0.0f, nullptr), SND_ERR_INVALID_HANDLE);
}

SND_TEST(CoreCommands, EmitterRowsAreReleasedBackToTheGame) {
  snd_config cfg = SmallConfig();
  cfg.max_emitters = 2;
  Harness h(cfg);
  for (int i = 0; i < 10; ++i) {
    const snd_emitter_handle a = h.sys->CreateEmitter();
    const snd_emitter_handle b = h.sys->CreateEmitter();
    SND_REQUIRE(a.value != 0u && b.value != 0u);
    SND_CHECK_EQ(h.sys->CreateEmitter().value, 0u);  // full
    h.sys->DestroyEmitter(a);
    h.sys->DestroyEmitter(b);
    h.Steps(2);
    h.sys->Update();
  }
}

namespace {
std::vector<float> RunScript(bool parallel) {
  snd_config cfg = SmallConfig();
  cfg.jobs.realtime_workers = parallel ? 2u : 0u;
  cfg.tuning.rt_spin_microseconds = 200000;  // never late: the outputs must match bit for bit
  cfg.tuning.lod_down_blocks = 0;            // bit exactness needs the load LOD off (2.5); slow builds would trigger it
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("s"), 1, 44100, Sine(200000, 330.0, 44100.0, 0.3f), 0, 0});
  b.assets.push_back(TAsset{HashId("t"), 1, 48000, Sine(200000, 1234.0, 48000.0, 0.2f), 0, 0});
  TEvent e;
  e.id = HashId("R");
  e.sequence = static_cast<std::uint32_t>(bf::SequenceKind::Random);
  TTrack tr;
  TClip c0;
  c0.target = 0;
  c0.randPitchCents = 300.0f;
  c0.randVolumeDb = 6.0f;
  c0.randPan = 1.0f;
  TClip c1 = c0;
  c1.target = 1;
  tr.clips = {c0, c1};
  e.tracks.push_back(tr);
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  for (int i = 0; i < 12; ++i) {
    h.sys->Play(HashId("R"), snd_emitter_handle{0}, nullptr);
    h.Steps(3);
  }
  h.Steps(10);
  return h.all;
}
}  // namespace

SND_TEST(CoreDeterminism, SameScriptSameBits) {
  const std::vector<float> a = RunScript(false);
  const std::vector<float> b = RunScript(false);
  SND_REQUIRE_EQ(a.size(), b.size());
  SND_CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
  // Parallel rendering adds the chunks in chunk order (3.4): identical output.
  const std::vector<float> c = RunScript(true);
  SND_REQUIRE_EQ(a.size(), c.size());
  double maxDiff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, double(std::fabs(a[i] - c[i])));
  snd::test::Note("serial vs parallel max |diff| = %g", maxDiff);
  SND_CHECK(maxDiff <= 1e-6);
  double energy = 0;
  for (float v : a) energy += double(v) * v;
  SND_CHECK(energy > 1.0);
}

SND_TEST(CoreMemory, NoAllocationAfterInitialisation) {
  CountingAllocator alloc;
  {
    Harness h(SmallConfig(), {}, nullptr, 0, &alloc);
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(4000, 0.1f), 0, 0});
    b.events.push_back(SimpleEvent("E", 0));
    h.Load("b", b.Build(HashId("B")));
    h.Steps(2);
    const std::uint64_t before = alloc.allocations.load();
    const snd_emitter_handle em = h.sys->CreateEmitter();
    for (int i = 0; i < 100; ++i) {
      h.sys->Play(HashId("E"), em, nullptr);
      h.sys->SetParamEmitter(em, HashId("X"), float(i));
      h.sys->SetBusVolume(HashId("Sfx"), 0.5f + 0.001f * i, 0.01f);
      h.Steps(1);
    }
    SND_CHECK_EQ(alloc.allocations.load(), before);
  }
  SND_CHECK_EQ(alloc.allocations.load(), alloc.frees.load());
}

SND_TEST(CoreRt, RenderPathHasNoRtViolations) {
  snd_config cfg = SmallConfig();
  cfg.jobs.realtime_workers = 2;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 44100, Sine(48000, 440.0, 44100.0), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  snd::test::rt::Reset();
  for (int i = 0; i < 40; ++i) {
    h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
    h.sys->Update();
    {
      SND_RT_SCOPE();
      h.sys->RenderBlock(h.out.data());  // RenderBlock opens its own scope as well
    }
  }
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}

SND_TEST(CoreBanks, LanguageSwitchKeepsPlayingVoicesAndResolvesTheNewLanguage) {
  Harness h;
  auto voiceBank = [](const char* lang, float level, std::size_t frames) {
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("line"), 1, 48000, Constant(frames, level), 0, 0});
    b.events.push_back(SimpleEvent("Line", 0));
    return b.Build(HashId("Voice"), HashId(lang));
  };
  SND_CHECK_EQ(h.sys->DeclareLanguage("en"), SND_OK);
  SND_CHECK_EQ(h.sys->DeclareLanguage("ja"), SND_OK);
  SND_CHECK_EQ(h.sys->SetLanguage(HashId("fr")), SND_ERR_NOT_FOUND);
  h.fs.AddFile("Voice.ja.sndb", voiceBank("ja", 0.2f, 480000));
  const snd_bank_handle en = h.Load("Voice.en.sndb", voiceBank("en", 0.1f, 20000));
  SND_REQUIRE_EQ(h.sys->GetBankState(en), SND_BANK_STATE_LOADED);
  const snd_playback_handle old = h.sys->Play(HashId("Line"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  SND_CHECK_EQ(h.sys->SetLanguage(HashId("ja")), SND_OK);
  h.Clear();
  h.Steps(10);
  // The playing English line keeps playing (10.4) ...
  SND_CHECK_EQ(h.ends.CountOf(old), 0u);
  SND_CHECK_NEAR(h.At(9 * 256, 0), 0.1f * kCenter, 1e-4f);
  // ... while new plays resolve to the Japanese bank.
  const snd_playback_handle neu = h.sys->Play(HashId("Line"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(h.At(2 * 256 + 10, 0), (0.1f + 0.2f) * kCenter, 1e-4f);
  // Once the old line ended, the English bank goes away.
  h.Steps(100);
  SND_CHECK(h.ends.Has(old, SND_END_COMPLETED));
  SND_CHECK_EQ(h.sys->GetBankState(en), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(h.ends.CountOf(neu), 0u);
}

SND_TEST(CoreQueues, A005_OverflowControlKeepsProducerOrderWithZeroReserve) {
  auto cfg = SmallConfig();
  cfg.commands.arena_bytes = 256;
  cfg.commands.reserve_bytes = 0;
  CountingAllocator alloc;
  Harness h(cfg, {}, nullptr, 0, &alloc);
  SND_REQUIRE_EQ(h.created, SND_OK);
  h.Steps(4);
  const auto allocations = alloc.allocations.load();
  // Every packet is 8 bytes. Exactly 32 fit without touching any fallback table.
  auto fill = [&] {
    for (int i = 0; i < 32; ++i) SND_REQUIRE_EQ(h.sys->Suspend(), SND_OK);
  };
  for (int pass = 0; pass < 2; ++pass) {
    std::thread worker([&] {
      SND_REQUIRE_EQ(h.sys->RegisterThread(), SND_OK);
      fill();
      SND_CHECK_EQ(h.sys->Resume(), SND_OK);
      if (pass == 1) SND_CHECK_EQ(h.sys->Suspend(), SND_OK);
      SND_CHECK_EQ(h.sys->Flush(), SND_OK);
      SND_CHECK_EQ(h.sys->UnregisterThread(), SND_OK);
    });
    worker.join();  // worker publishes first, but producer zero is applied first
    fill();
    SND_REQUIRE_EQ(h.sys->Resume(), SND_OK);
    if (pass == 0) SND_REQUIRE_EQ(h.sys->Suspend(), SND_OK);
    h.Steps(3);
    h.sys->Update();
    snd_stats a{}, b{};
    SND_REQUIRE_EQ(h.sys->GetStats(&a), SND_OK);
    h.Steps(3);
    h.sys->Update();
    SND_REQUIRE_EQ(h.sys->GetStats(&b), SND_OK);
    if (pass == 0) SND_CHECK(b.clock_sample > a.clock_sample);
    else SND_CHECK_EQ(b.clock_sample, a.clock_sample);
  }
  SND_CHECK_EQ(alloc.allocations.load(), allocations);
}
