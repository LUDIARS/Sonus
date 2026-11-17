// Independent tests of core that FAIL ON PURPOSE: each documents a defect
// found by the independent tester (stage 2). One executable per defect
// (CMake defines SND_INDEP_DEFECT_<name>), so a defect never hides others.
// Do not "fix" the expectations; fix core.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_fixture.hpp"

using namespace indep;
using snd::HashId;

namespace {
[[maybe_unused]] std::vector<std::uint8_t> OneEvent(const char* name, float f, std::uint32_t priority = 128) {
  Bank b;
  b.Event(name, f, priority);
  return b.Build("B");
}
}  // namespace

#if defined(SND_INDEP_DEFECT_record_order)
// C2 with the reserve region (commands.hpp: "normal, reserve, fallback ...
// equals the recording order"). A critical Play that no longer fits the normal
// region goes to the reserve; a smaller Stop recorded after it still fits the
// normal region and is applied first, i.e. before its Play.
SND_TEST(IndepDefect, Defect_core_record_order_reserve_before_normal) {
  snd_config cfg = Config();
  cfg.commands.arena_bytes = 4096;
  cfg.commands.reserve_bytes = 1024;
  Fx fx(cfg);
  SND_REQUIRE_EQ(fx.created, SND_OK);
  fx.Load("b", OneEvent("Crit", 500.0f, 250));  // priority >= criticalPriority (224)
  fx.Steps(2);
  snd_playback_handle p{0};
  snd_result stop = SND_ERR_INVALID_ARGUMENT;
  std::thread t([&] {
    SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);
    // 126 x 32 B = 4032 B: 64 B left in the normal region.
    for (int i = 0; i < 126; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);
    p = fx.sys->Play(HashId("Crit"), NoEmitter(), nullptr);  // 80 B -> reserve (critical)
    stop = fx.sys->Stop(p, 0.0f, nullptr);                   // 56 B -> normal
    fx.sys->UnregisterThread();
  });
  t.join();
  SND_REQUIRE(p.value != 0u);
  SND_CHECK_EQ(stop, SND_OK);
  fx.Steps(10);
  // Recorded Play then Stop: the playback must be stopped and silent by now.
  SND_CHECK_MSG(fx.Ended(p.value, SND_END_STOPPED), "playback recorded Play->Stop is still alive");
  SND_CHECK_MSG(fx.LastMean() == 0.0f, "still audible: %f", static_cast<double>(fx.LastMean()));
}
#endif

#if defined(SND_INDEP_DEFECT_unregister)
// sonus.h: snd_unregister_thread "Publish pending commands and unregister".
// A thread that flushed more often than the audio thread returned arenas still
// holds unpublished commands in its current arena; Unregister must not drop
// them (must-deliver commands are never silently lost, D-09).
SND_TEST(IndepDefect, Defect_core_unregister_loses_unpublished_commands) {
  snd_config cfg = Config();
  cfg.max_real_voices = 64;
  cfg.max_playbacks = 256;
  Bank b;
  b.Event("Short", 2000.0f + 512.0f);
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  std::vector<std::uint64_t> hs;
  std::thread t([&] {
    SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);
    for (int i = 0; i < 40; ++i) {
      hs.push_back(fx.sys->Play(HashId("Short"), NoEmitter(), nullptr).value);
      if ((i % 10) == 9) SND_CHECK_EQ(fx.sys->Flush(), SND_OK);  // 4 publishes, 3 arenas, no block in between
    }
    SND_CHECK_EQ(fx.sys->UnregisterThread(), SND_OK);
  });
  t.join();
  fx.Steps(20);
  std::uint32_t ended = 0;
  for (std::uint64_t h : hs) ended += fx.CountEnds(h) == 1u ? 1u : 0u;
  SND_CHECK_MSG(ended == 40u, "only %u of 40 plays recorded before UnregisterThread arrived", ended);
  SND_CHECK_EQ(fx.kind.starts.load(), 40u);
}
#endif

#if defined(SND_INDEP_DEFECT_timed_stop)
// C12: a Stop / SetScheduledStop at a clock keeps the sound at full level up
// to that clock, then it fades (short fade) and the playback ends STOPPED.
SND_TEST(IndepDefect, Defect_core_timed_stop_fades_before_its_clock) {
  for (int variant = 0; variant < 2; ++variant) {
    Fx fx;
    fx.Load("b", OneEvent("E", 500.0f));
    const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(2);
    const std::size_t x = fx.Total() + 4 * 256 + 130;
    const snd_when w = AtClock(x);
    if (variant == 0) {
      SND_CHECK_EQ(fx.sys->Stop(p, 0.0f, &w), SND_OK);
    } else {
      SND_CHECK_EQ(fx.sys->SetScheduledStop(p, &w, 0.0f), SND_OK);
    }
    fx.Steps(12);
    float minBefore = 1.0f;
    for (std::size_t f = x - 120; f < x; ++f) minBefore = fx.At(f) < minBefore ? fx.At(f) : minBefore;
    SND_CHECK_MSG(std::fabs(minBefore - kCenter * 0.5f) < 1e-5f, "variant %d: level %f before the stop clock", variant,
                  static_cast<double>(minBefore));
    SND_CHECK_EQ(fx.MaxAbs(x + 256 + 240, fx.Total()), 0.0f);
    SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  }
}

#endif

#if defined(SND_INDEP_DEFECT_epoch_untracked)
// 3.3 / core.hpp "releases unloaded banks once ... source jobs posted before
// the unregistration finished". When the job-wrap pool (2 x serviceQueueEntries)
// is exhausted, PumpServices submits jobs untracked (services.cpp) and the
// epoch no longer waits for them: the bank is released while such a job has
// not run yet.
SND_TEST(IndepDefect, Defect_core_epoch_ignores_untracked_jobs) {
  Bank b;
  b.Event("E", 500.0f);
  snd::core::CoreLimits lim;
  lim.serviceQueueEntries = 16;  // queue 16 per pump, 32 job wraps
  Fx fx(Config(), lim);
  SND_REQUIRE_EQ(fx.created, SND_OK);
  const snd_bank_handle h = fx.Load("b", b.Build("B"));
  fx.jobs.SetMode(snd::mock::JobMode::Manual);
  for (int round = 0; round < 3; ++round) {  // 3 x 16 jobs: 32 tracked, 16 untracked
    fx.kind.postJobs.store(16);
    fx.Step();
  }
  fx.sys->Update();
  SND_REQUIRE_EQ(fx.jobs.Pending(), 48u);
  fx.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD);
  for (int i = 0; i < 32; ++i) fx.jobs.RunNext();  // the 32 tracked ones (FIFO)
  fx.Steps(30);
  SND_CHECK_EQ(fx.jobs.Pending(), 16u);
  SND_CHECK_MSG(fx.sys->GetBankState(h) == SND_BANK_STATE_UNLOADING,
                "bank released (state %u) while 16 source jobs posted before the unload have not run",
                fx.sys->GetBankState(h));
  fx.jobs.RunAll();
  fx.Steps(5);
}
#endif
