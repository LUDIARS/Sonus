// Independent tests of core that FAIL ON PURPOSE: each documents a defect
// found by the independent tester (stage 2). One executable per defect
// (CMake defines SND_INDEP_DEFECT_<name>), so a defect never hides others.
// Do not "fix" the expectations; fix core.
#include <atomic>
#include <chrono>
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

#if defined(SND_INDEP_DEFECT_control_held)
// D-43 (3) / source_if.hpp: while a voice is held "no system reads or writes
// its render state" (the source instance included). Pausing a playback whose
// voice is held sends ISourceKind::Control(Pause) for that voice while the late
// chunk may still be inside RenderBatch for the same source slot
// (sys_event.cpp SetVoicesPaused does not skip held voices).
SND_TEST(IndepDefect, Defect_core_control_on_held_voice) {
  snd_config cfg = Config();
  cfg.max_real_voices = 8;
  cfg.jobs.realtime_workers = 1;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 50;
  Bank b;
  std::vector<snd_playback_handle> ps;
  for (int i = 0; i < 8; ++i) {
    char n[8];
    std::snprintf(n, sizeof(n), "C%d", i);
    b.Event(n, 1000.0f);
  }
  Fx fx(cfg);
  fx.Load("p", b.Build("P"));
  for (int i = 0; i < 8; ++i) {
    char n[8];
    std::snprintf(n, sizeof(n), "C%d", i);
    ps.push_back(fx.sys->Play(HashId(n), NoEmitter(), nullptr));
  }
  fx.Steps(4);
  std::uint32_t lateSeen = 0;
  for (int tries = 0; tries < 20 && fx.kind.controlBusy.load() == 0u; ++tries) {
    fx.kind.slowNs.store(30000000);  // 30 ms: the worker's chunk stays in RenderBatch
    fx.Step();
    fx.kind.slowNs.store(0);
    if (fx.sys->LastBlock().lateChunks == 0u) continue;
    ++lateSeen;
    for (const snd_playback_handle& p : ps) fx.sys->Pause(p, 0.0f);
    fx.Steps(3);  // unpaced: well inside the 30 ms
    for (const snd_playback_handle& p : ps) fx.sys->ResumePlayback(p, 0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    fx.Steps(4);
  }
  SND_REQUIRE(lateSeen > 0u);
  SND_CHECK_MSG(fx.kind.controlBusy.load() == 0u, "Control on a source slot inside a late RenderBatch: %llu",
                static_cast<unsigned long long>(fx.kind.controlBusy.load()));
}
#endif

#if defined(SND_INDEP_DEFECT_language_back)
// 10.4: switching the language back while the previous language's bank still
// waits to be unloaded (a voice of it plays): after SetLanguage(en) new plays
// must resolve to English again. Core keeps the old English copy on its way out
// (unload when unused), loads a second English copy that waits for that, and
// meanwhile resolves new plays to Japanese although English is current.
SND_TEST(IndepDefect, Defect_core_language_switch_back_while_old_bank_unloading) {
  auto voice = [](const char* lang, float f) {
    Bank b;
    b.Event("Line", f);
    return b.Build("Voice", HashId(lang));
  };
  Fx fx;
  SND_CHECK_EQ(fx.sys->DeclareLanguage("en"), SND_OK);
  SND_CHECK_EQ(fx.sys->DeclareLanguage("ja"), SND_OK);
  SND_CHECK_EQ(fx.sys->DeclareLanguage("en"), SND_OK);  // again: no-op
  fx.fs.AddFile("Voice.ja.sndb", voice("ja", 2000.0f + 256.0f * 200.0f));  // DC 1.0, 200 blocks
  const snd_bank_handle en = fx.Load("Voice.en.sndb", voice("en", 100.0f));  // DC 0.1 endless
  SND_REQUIRE_EQ(fx.sys->GetBankState(en), SND_BANK_STATE_LOADED);
  const snd_playback_handle a = fx.sys->Play(HashId("Line"), NoEmitter(), nullptr);
  fx.Steps(2);
  SND_CHECK_EQ(fx.sys->SetLanguage(HashId("ja")), SND_OK);
  fx.Steps(10);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.1f, 1e-5f);  // A keeps English
  const snd_playback_handle b = fx.sys->Play(HashId("Line"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 1.1f, 1e-5f);  // B is Japanese
  // (en may show UNLOADING here: it waits until A no longer uses it)
  // Back to English while both play.
  SND_CHECK_EQ(fx.sys->SetLanguage(HashId("en")), SND_OK);
  fx.Steps(10);
  const snd_playback_handle c = fx.sys->Play(HashId("Line"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 1.2f, 1e-5f);  // C is English again
  fx.sys->Stop(a, 0.0f, nullptr);
  fx.sys->Stop(c, 0.0f, nullptr);
  fx.Steps(4);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 1.0f, 1e-5f);
  SND_CHECK_EQ(fx.CountEnds(b.value), 0u);
  fx.Steps(220);
  SND_CHECK(fx.Ended(b.value, SND_END_COMPLETED));
  // English is the current language: new plays are English (the English bank
  // may be a new copy with another handle by now).
  fx.sys->Play(HashId("Line"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.1f, 1e-5f);
}

#endif
