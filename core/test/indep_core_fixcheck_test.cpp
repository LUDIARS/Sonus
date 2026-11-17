// Independent re-check of the stage-2 fixes (38e9fc9..878307b): do the fixes
// keep their own promises at the edges, and did they bring new defects?
//   1. reserve / fallback boundary and recording order (D-11, commands.hpp)
//   2. retired producers and producer numbers (UnregisterThread)
// Tests that found new defects live in indep_core_defects_test.cpp.
//   3. timed stops overlapping plain stops / pauses / later schedules
//   4. epoch and Destroy (every job, tracked from postJob; Destroy waits)
//   5. held voices: the final pause state is applied once after the hold
//   6. language back and forth, repeatedly
//   7. start clocks at the block edges
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
std::vector<std::uint8_t> OneEvent(const char* name, float f, std::uint32_t priority = 128) {
  Bank b;
  b.Event(name, f, priority);
  return b.Build("B");
}
snd_config SmallArenas(std::uint32_t reserve = 1024) {
  snd_config c = Config();
  c.commands.arena_bytes = 4096;
  c.commands.reserve_bytes = reserve;
  c.commands.overflow_param_slots = 4;
  return c;
}
}  // namespace

// 1a. Once a critical Play went to the reserve, a later Pause (24 B, it would
// fit the normal region) follows it: the playback is paused.
SND_TEST(IndepFix, LaterSmallCommandFollowsTheReserve) {
  Fx fx(SmallArenas());
  fx.Load("b", OneEvent("Crit", 500.0f, 250));
  fx.Steps(2);
  snd_playback_handle p{0};
  std::thread t([&] {
    SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);
    for (int i = 0; i < 126; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);  // 64 B left
    p = fx.sys->Play(HashId("Crit"), NoEmitter(), nullptr);                     // reserve
    SND_CHECK_EQ(fx.sys->Pause(p, 0.0f), SND_OK);                               // must follow
    fx.sys->UnregisterThread();
  });
  t.join();
  fx.Steps(6);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PAUSED);
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
}

// 1b. Once the fallback is in use, the reserve takes nothing more: a Stop that
// did not fit the reserve is coalesced; a Pause recorded after it would still
// fit the reserve but has no fallback form, so it is refused (D-11) instead of
// overtaking the Stop. Refused commands are not "lost" (reserve_overflows).
// A refused create returns an invalid handle.
SND_TEST(IndepFix, FallbackClosesTheReserveAndRefusesWhatItCannotHold) {
  Fx fx(SmallArenas(256));
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle a = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  const snd_playback_handle b2 = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  snd_stats s0{};
  fx.sys->Update();
  fx.sys->GetStats(&s0);
  for (int i = 0; i < 128; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);  // normal full
  for (int i = 0; i < 6; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);    // reserve: 192 of 256 B
  SND_CHECK_EQ(fx.sys->SetBusVolume(HashId("Sfx"), 1.0f, 0.0f), SND_OK);       // reserve: 216 B, 40 B left
  SND_CHECK_EQ(fx.sys->Stop(b2, 0.0f, nullptr), SND_OK);  // 56 B > 40 B: coalesced, fallback in use
  // 24 B would fit the 40 B left, but the reserve is closed now (order) and
  // these have no fallback form: refused.
  SND_CHECK_EQ(fx.sys->Pause(a, 0.0f), SND_ERR_COMMAND_BUFFER_FULL);
  SND_CHECK_EQ(fx.sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.0f), SND_ERR_COMMAND_BUFFER_FULL);
  SND_CHECK_EQ(fx.sys->CreateEmitter().value, 0u);  // 16 B create: invalid handle
  fx.Steps(6);
  SND_CHECK(fx.Ended(b2.value, SND_END_STOPPED));
  SND_CHECK_EQ(fx.CountEnds(a.value), 0u);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(a), SND_PLAYBACK_STATE_PLAYING);  // the refused Pause did nothing
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);                  // nor the refused volume
  snd_stats s1{};
  fx.sys->Update();
  fx.sys->GetStats(&s1);
  SND_CHECK_EQ(s1.reserve_overflows, s0.reserve_overflows);
  // The next frame records normally again.
  const snd_playback_handle c = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  SND_CHECK_EQ(fx.sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.0f), SND_OK);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.5f + 0.5f) * 0.5f, 1e-5f);  // a + c at bus 0.5
  SND_CHECK_EQ(fx.CountEnds(c.value), 0u);
}

// 2b. Retired producers keep their number until published: with one producer
// number for other threads, a second thread cannot register while the first
// one's retired arena is pending; once Update published it, it can, and every
// command of both arrives.
SND_TEST(IndepFix, RetiredNumberIsNotReusedUntilPublished) {
  snd_config cfg = Config();
  cfg.max_registered_threads = 2;  // owner + 1
  cfg.max_playbacks = 256;
  cfg.max_real_voices = 64;
  Bank b;
  b.Event("Short", 2000.0f + 300.0f);
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  fx.Steps(2);
  std::vector<std::uint64_t> hs;
  std::thread t([&] {
    SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);
    for (int i = 0; i < 4; ++i) {
      hs.push_back(fx.sys->Play(HashId("Short"), NoEmitter(), nullptr).value);
      fx.sys->Flush();
    }
    SND_CHECK_EQ(fx.sys->UnregisterThread(), SND_OK);
  });
  t.join();
  snd_result second = SND_OK;
  std::thread u([&] { second = fx.sys->RegisterThread(); });
  u.join();
  SND_CHECK_EQ(second, SND_ERR_LIMIT_REACHED);  // the number is still retired
  fx.Steps(6);                                 // Update publishes and frees it
  std::thread w([&] {
    SND_CHECK_EQ(fx.sys->RegisterThread(), SND_OK);
    hs.push_back(fx.sys->Play(HashId("Short"), NoEmitter(), nullptr).value);
    fx.sys->UnregisterThread();
  });
  w.join();
  fx.Steps(10);
  for (std::uint64_t h : hs) SND_CHECK_EQ(fx.CountEnds(h), 1u);
  SND_CHECK_EQ(fx.kind.starts.load(), 5u);
}

// 3a. A timed stop at X, then a plain Stop before X: it stops now (no wait).
// 3b. A later SetScheduledStop replaces the earlier one.
// (3c, a pause before X: Defect_core_timed_stop_of_paused_playback_sounds)
SND_TEST(IndepFix, TimedStopMeetsPlainStopsPausesAndReplacement) {
  {
    Fx fx;
    fx.Load("b", OneEvent("E", 500.0f));
    const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(2);
    const snd_when far = AtClock(fx.Total() + 100 * 256);
    fx.sys->Stop(p, 0.5f, &far);
    fx.Steps(2);
    fx.sys->Stop(p, 0.0f, nullptr);
    fx.Steps(4);
    SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
    SND_CHECK_EQ(fx.LastMean(), 0.0f);
  }
  {
    Fx fx;
    fx.Load("b", OneEvent("E", 500.0f));
    const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
    fx.Steps(2);
    const std::size_t x1 = fx.Total() + 4 * 256 + 10, x2 = fx.Total() + 12 * 256 + 50;
    const snd_when w1 = AtClock(x1), w2 = AtClock(x2);
    fx.sys->SetScheduledStop(p, &w1, 0.0f);
    fx.sys->SetScheduledStop(p, &w2, 0.0f);  // replaces
    fx.Steps(20);
    SND_CHECK_NEAR(fx.At(x1 + 300), kCenter * 0.5f, 1e-5f);
    SND_CHECK_NEAR(fx.At(x2 - 1), kCenter * 0.5f, 1e-5f);
    SND_CHECK(fx.At(x2 + 1) < kCenter * 0.5f);  // fades from x2
    SND_CHECK_EQ(fx.MaxAbs(x2 + 256, fx.Total()), 0.0f);
    SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  }
}

// 3d. A timed stop with a fade (0.1 s): full level to X, then a monotone fade
// of ~0.1 s that starts at X.
SND_TEST(IndepFix, TimedStopFadeStartsExactlyAtX) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(2);
  const std::size_t x = fx.Total() + 3 * 256 + 200;
  const snd_when w = AtClock(x);
  fx.sys->Stop(p, 0.1f, &w);
  fx.Steps(30);
  SND_CHECK_NEAR(fx.At(x - 1), kCenter * 0.5f, 1e-6f);
  SND_CHECK(fx.At(x + 10) < kCenter * 0.5f);
  SND_CHECK_NEAR(fx.At(x + 2400), kCenter * 0.25f, 0.02f);
  std::size_t zero = 0;
  for (std::size_t f = x; f < fx.Total() && zero == 0; ++f) zero = fx.At(f) == 0.0f ? f : 0u;
  SND_CHECK_MSG(zero > x + 4500 && zero < x + 5200, "fade length %zu frames", zero - x);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
}

// 4a. Destroy waits for every job posted by sources (jobs run late, on another thread).
SND_TEST(IndepFix, DestroyWaitsForPendingJobs) {
  Bank b;
  b.Event("E", 500.0f);
  auto* fx = new Fx();
  fx->Load("b", b.Build("B"));
  fx->jobs.SetMode(snd::mock::JobMode::Manual);
  fx->kind.postJobs.store(5);
  fx->Steps(2);
  fx->sys->Update();
  std::atomic<bool> ran{false};
  std::thread helper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fx->jobs.RunAll();
    ran = true;
  });
  snd::core::System::Destroy(fx->sys);  // must not return before the jobs ran
  const bool ranAtReturn = ran.load();
  const std::uint64_t jobsAtReturn = fx->kind.jobsRun.load();
  helper.join();
  fx->sys = nullptr;
  SND_CHECK(ranAtReturn);
  SND_CHECK_EQ(jobsAtReturn, fx->kind.posted.load());
  delete fx;
}

// 4b. With the job records exhausted postJob refuses (no untracked job): the
// epoch still waits for every accepted job, and nothing is lost silently.
SND_TEST(IndepFix, EpochWithManyJobsInFlight) {
  Bank b;
  b.Event("E", 500.0f);
  snd::core::CoreLimits lim;
  lim.serviceQueueEntries = 16;
  Fx fx(Config(), lim);
  const snd_bank_handle h = fx.Load("b", b.Build("B"));
  fx.jobs.SetMode(snd::mock::JobMode::Manual);
  for (int round = 0; round < 8; ++round) {
    fx.kind.postJobs.store(16);
    fx.Step();
  }
  fx.sys->Update();
  const std::uint64_t accepted = fx.kind.posted.load();
  SND_CHECK_EQ(fx.jobs.Pending(), accepted);  // every accepted job was submitted
  fx.sys->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD);
  for (std::uint64_t i = 0; i + 1 < accepted; ++i) fx.jobs.RunNext();
  fx.Steps(20);
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_UNLOADING);  // one job left
  fx.jobs.RunAll();
  fx.Steps(5);
  SND_CHECK_EQ(fx.sys->GetBankState(h), SND_BANK_STATE_UNLOADED);
  std::printf("accepted %llu jobs of %d posted\n", static_cast<unsigned long long>(accepted), 8 * 16);
}

// 5. Pause -> Resume -> Pause while voices are held: after the hold the final
// state (paused) is applied once to each held voice: one Pause, no Resume.
SND_TEST(IndepFix, HeldVoicesGetTheFinalPauseStateOnce) {
  snd_config cfg = Config();
  cfg.max_real_voices = 8;
  cfg.jobs.realtime_workers = 1;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 50;
  cfg.tuning.lod_down_blocks = 0;
  Bank b;
  for (int i = 0; i < 8; ++i) {
    char n[8];
    std::snprintf(n, sizeof(n), "C%d", i);
    b.Event(n, 1000.0f);
  }
  bool checked = false;
  for (int attempt = 0; attempt < 30 && !checked; ++attempt) {
    Fx fx(cfg);
    fx.Load("p", b.Build("P"));
    std::vector<snd_playback_handle> ps;
    for (int i = 0; i < 8; ++i) {
      char n[8];
      std::snprintf(n, sizeof(n), "C%d", i);
      ps.push_back(fx.sys->Play(HashId(n), NoEmitter(), nullptr));
    }
    fx.Steps(4);
    fx.kind.slowNs.store(40000000);  // 40 ms in the worker's chunks
    fx.kind.callerSlowNs.store(300000);
    fx.Step();
    fx.kind.slowNs.store(0);
    fx.kind.callerSlowNs.store(0);
    if (fx.sys->LastBlock().lateChunks == 0u) continue;
    const std::uint64_t heldN = fx.sys->LastBlock().heldVoices;  // one voice per chunk
    const std::uint64_t pause0 = fx.kind.controlsByOp[static_cast<int>(snd::source::ControlOp::Pause)].load();
    const std::uint64_t resume0 = fx.kind.controlsByOp[static_cast<int>(snd::source::ControlOp::Resume)].load();
    for (auto& p : ps) fx.sys->Pause(p, 0.0f);
    fx.Steps(3);
    for (auto& p : ps) fx.sys->ResumePlayback(p, 0.0f);
    fx.Steps(3);
    for (auto& p : ps) fx.sys->Pause(p, 0.0f);
    fx.Steps(3);  // all inside the 40 ms
    const bool stillHeld = fx.sys->LastBlock().heldVoices != 0u;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    fx.Steps(8);
    fx.sys->Update();
    if (!stillHeld) continue;
    for (auto& p : ps) SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PAUSED);
    SND_CHECK_EQ(fx.LastMean(), 0.0f);
    const std::uint64_t pauses = fx.kind.controlsByOp[static_cast<int>(snd::source::ControlOp::Pause)].load() - pause0;
    const std::uint64_t resumes = fx.kind.controlsByOp[static_cast<int>(snd::source::ControlOp::Resume)].load() - resume0;
    // Voices that were not held follow every step (the 0 s pause completes
    // within the 3 blocks): Pause, Resume, Pause. Held voices get the final
    // state once: one Pause, no Resume.
    const std::uint64_t free = 8u - heldN;
    SND_CHECK_MSG(pauses == 2u * free + heldN && resumes == free,
                  "%llu held: %llu Pause / %llu Resume controls (expected %llu / %llu)",
                  static_cast<unsigned long long>(heldN), static_cast<unsigned long long>(pauses),
                  static_cast<unsigned long long>(resumes), static_cast<unsigned long long>(2u * free + heldN),
                  static_cast<unsigned long long>(free));
    SND_CHECK_EQ(fx.kind.controlBusy.load(), 0u);
    SND_CHECK_EQ(fx.kind.discont.load(), 0u);
    // And resuming afterwards works.
    for (auto& p : ps) fx.sys->ResumePlayback(p, 0.0f);
    fx.Steps(4);
    SND_CHECK(fx.LastMean() > 0.0f);
    checked = true;
  }
  if (!checked) std::printf("SKIP: no hold that lasted over the pause sequence in 30 attempts\n");
}

// 6. en -> ja -> en -> ja -> en with voices of each playing: every new play
// uses the current language; when everything stopped only one copy of the
// language bank is left (bank memory back to one bank).
SND_TEST(IndepFix, LanguageBackAndForthRepeatedly) {
  auto voice = [](const char* lang, float f) {
    Bank b;
    b.Event("Line", f);
    return b.Build("Voice", HashId(lang));
  };
  Fx fx;
  fx.sys->DeclareLanguage("en");
  fx.sys->DeclareLanguage("ja");
  fx.fs.AddFile("Voice.ja.sndb", voice("ja", 200.0f));  // same size as en
  fx.Load("Voice.en.sndb", voice("en", 100.0f));
  fx.Steps(2);
  fx.sys->Update();
  snd_stats base{};
  fx.sys->GetStats(&base);
  const float level[2] = {0.1f, 0.2f};
  const char* names[2] = {"en", "ja"};
  std::vector<snd_playback_handle> ps;
  float sum = 0.0f;
  ps.push_back(fx.sys->Play(HashId("Line"), NoEmitter(), nullptr));
  sum += level[0];
  fx.Steps(3);
  for (int k = 1; k <= 4; ++k) {
    const int lang = k % 2;
    SND_CHECK_EQ(fx.sys->SetLanguage(HashId(names[lang])), SND_OK);
    fx.Steps(12);
    ps.push_back(fx.sys->Play(HashId("Line"), NoEmitter(), nullptr));
    sum += level[lang];
    fx.Steps(3);
    SND_CHECK_MSG(std::fabs(fx.LastMean() - kCenter * sum) < 1e-5f, "switch %d (%s): level %f, expected %f", k,
                  names[lang], static_cast<double>(fx.LastMean() / kCenter), static_cast<double>(sum));
  }
  for (auto& p : ps) fx.sys->Stop(p, 0.0f, nullptr);
  fx.Steps(40);
  fx.sys->Update();
  snd_stats end{};
  fx.sys->GetStats(&end);
  SND_CHECK_EQ(end.memory_used[SND_MEM_BANK], base.memory_used[SND_MEM_BANK]);
  const snd_playback_handle last = fx.sys->Play(HashId("Line"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * level[0], 1e-5f);  // English is current
  SND_CHECK_EQ(fx.CountEnds(last.value), 0u);
}

// 7. Start clocks at the edges of the apply block: its first frame, its last
// frame, one frame before it (past: block start + warning), the next block's
// first frame, and DELAY_US 0.
SND_TEST(IndepFix, StartClocksAtTheBlockEdges) {
  for (int variant = 0; variant < 5; ++variant) {
    Fx fx;
    fx.Load("b", OneEvent("E", 500.0f));
    fx.Steps(3);
    const std::size_t s = fx.Total();  // the next RenderBlock applies the command
    snd_when w{};
    std::size_t at = s;
    switch (variant) {
      case 0: w = AtClock(s); break;
      case 1: w = AtClock(s + 255); at = s + 255; break;
      case 2: w = AtClock(s - 1); break;
      case 3: w = AtClock(s + 256); at = s + 256; break;
      default: w.kind = SND_WHEN_DELAY_US; w.value = 0; break;
    }
    const std::size_t errs = fx.errors.size();
    fx.sys->Play(HashId("E"), NoEmitter(), &w);
    fx.Steps(4);
    if (at > s) SND_CHECK_MSG(fx.At(at - 1) == 0.0f, "variant %d: early", variant);
    SND_CHECK_MSG(std::fabs(fx.At(at) - kCenter * 0.5f) < 1e-6f, "variant %d: not at full level at its clock", variant);
    if (variant == 2) SND_CHECK_MSG(fx.errors.size() > errs, "past clock without a warning");
    if (variant == 0 || variant == 1 || variant == 3) SND_CHECK_EQ(fx.errors.size(), errs);
  }
}
