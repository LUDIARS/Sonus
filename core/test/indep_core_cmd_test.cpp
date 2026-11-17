// Independent tests: commands (2.1, D-09..D-12, commands.hpp), sample-accurate
// start / stop (4.4, D-21), the playback state machine (4.1), pause / resume,
// suspend (2.6), Render(frames) (core.hpp). Expected values are analytic
// (DC / counter sources of indep_fixture.hpp).
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

std::vector<std::uint8_t> OneEvent(const char* name, float f, std::uint32_t priority = 128) {
  Bank b;
  b.Event(name, f, priority);
  return b.Build("B");
}

}  // namespace

// C1: recording does nothing until the arena is published (Update / Flush).
SND_TEST(IndepCmd, NothingIsAppliedBeforeThePublish) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  SND_REQUIRE(p.value != 0u);
  for (int i = 0; i < 3; ++i) fx.Render();  // no Update: not published
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
  fx.Steps(2);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);
}

// Calibration: a mono DC voice on the centre gives constant power (L == R).
SND_TEST(IndepCmd, CentrePanIsConstantPower) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  const float l = fx.LastMean(0), r = fx.LastMean(1);
  SND_CHECK_NEAR(l, r, 1e-6f);
  SND_CHECK_NEAR(l * l + r * r, 0.25f, 1e-5f);
}

// C2: within one producer the later value of the same parameter wins.
SND_TEST(IndepCmd, SameProducerLastParameterValueWins) {
  Bank b;
  b.b.params.push_back(TParam{HashId("Vol"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  b.b.curves.push_back(TCurve{HashId("Vol"), 0u, 0u, {{0.0f, 0.0f}, {1.0f, -20.0f}}});
  TEvent& e = b.Event("E", 500.0f);
  e.curves.push_back(0);
  Fx fx;
  fx.Load("b", b.Build("B"));
  fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.sys->SetParamGlobal(HashId("Vol"), 0.0f);
  fx.sys->SetParamGlobal(HashId("Vol"), 1.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * 0.1f, 1e-5f);
  fx.sys->SetParamGlobal(HashId("Vol"), 1.0f);
  fx.sys->SetParamGlobal(HashId("Vol"), 0.0f);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);
}

// C2 / P3: Play then Stop recorded in the same frame: the stop applies after
// the play (same producer, record order), so at most a short fade is heard.
SND_TEST(IndepCmd, PlayThenStopInTheSameFrameEndsStopped) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  SND_CHECK_EQ(fx.sys->Stop(p, 0.0f, nullptr), SND_OK);
  const std::size_t from = fx.Total();
  fx.Steps(6);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  SND_CHECK_EQ(fx.CountEnds(p.value), 1u);
  SND_CHECK_EQ(fx.MaxAbs(from + 2 * fx.Frames(), fx.Total()), 0.0f);
}


// C3 / C4: normal region full -> a play is rejected (valid handle, REJECTED /
// COMMAND_OVERFLOW once), must-deliver commands still arrive through the reserve.
SND_TEST(IndepCmd, FullNormalRegionRejectsPlaysButDeliversMustDeliver) {
  snd_config cfg = Config();
  cfg.commands.arena_bytes = 4096;
  cfg.commands.reserve_bytes = 1024;
  Fx fx(cfg);
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle keep = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  snd_stats s0{};
  fx.sys->GetStats(&s0);
  for (int i = 0; i < 128; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);  // exactly full
  const snd_playback_handle rej = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  SND_REQUIRE(rej.value != 0u);
  SND_CHECK_EQ(fx.sys->SetBusVolume(HashId("Sfx"), 0.25f, 0.0f), SND_OK);  // reserve
  const snd_vec3 pos{1, 2, 3};
  const snd_emitter_handle em = fx.sys->CreateEmitter();  // must-deliver -> reserve
  SND_CHECK(em.value != 0u);
  fx.Steps(4);
  SND_CHECK(fx.Ended(rej.value, SND_END_REJECTED, SND_END_DETAIL_COMMAND_OVERFLOW));
  SND_CHECK_EQ(fx.CountEnds(rej.value), 1u);
  SND_CHECK_EQ(fx.CountEnds(keep.value), 0u);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * 0.25f, 1e-5f);
  // The emitter exists on the audio side: a transform on it is accepted.
  SND_CHECK_EQ(fx.sys->SetTransform(em, &pos, nullptr, nullptr), SND_OK);
  snd_stats s1{};
  fx.sys->GetStats(&s1);
  SND_CHECK(s1.rejected_commands > s0.rejected_commands);
}

// C6: droppable commands are dropped (and counted) when the normal region is full.
SND_TEST(IndepCmd, DroppableCommandsAreDroppedWhenFull) {
  snd_config cfg = Config();
  cfg.commands.arena_bytes = 4096;
  cfg.commands.reserve_bytes = 1024;
  Fx fx(cfg);
  const snd_emitter_handle em = fx.sys->CreateEmitter();
  fx.Steps(2);
  snd_stats s0{};
  fx.sys->GetStats(&s0);
  for (int i = 0; i < 128; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);
  const snd_vec3 pos{1, 0, 0};
  SND_CHECK_EQ(fx.sys->SetTransform(em, &pos, nullptr, nullptr), SND_ERR_COMMAND_BUFFER_FULL);
  snd_listener_desc l{};
  l.enabled = 1;
  SND_CHECK_EQ(fx.sys->SetListener(0, &l), SND_ERR_COMMAND_BUFFER_FULL);
  fx.Steps(2);
  snd_stats s1{};
  fx.sys->GetStats(&s1);
  SND_CHECK_EQ(s1.rejected_commands, s0.rejected_commands + 2u);
}

// C5 (D-11): with both regions full, Stop and parameter values are coalesced
// into the fallback and still applied; only values beyond overflowParamSlots
// are lost and counted.
SND_TEST(IndepCmd, FallbackDeliversStopAndLastParameterValues) {
  snd_config cfg = Config();
  cfg.commands.arena_bytes = 4096;
  cfg.commands.reserve_bytes = 256;
  cfg.commands.overflow_param_slots = 4;
  Bank b;
  b.b.params.push_back(TParam{HashId("Vol"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  b.b.curves.push_back(TCurve{HashId("Vol"), 0u, 0u, {{0.0f, 0.0f}, {1.0f, -20.0f}}});
  b.Event("Stopped", 300.0f);
  TEvent& q = b.Event("Q", 500.0f);
  q.curves.push_back(0);
  Fx fx(cfg);
  SND_REQUIRE_EQ(fx.created, SND_OK);
  fx.Load("b", b.Build("B"));
  const snd_playback_handle p = fx.sys->Play(HashId("Stopped"), NoEmitter(), nullptr);
  const snd_playback_handle qh = fx.sys->Play(HashId("Q"), NoEmitter(), nullptr);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * (0.3f + 0.5f), 1e-5f);
  snd_stats s0{};
  fx.sys->GetStats(&s0);
  for (int i = 0; i < 128 + 8; ++i) fx.sys->SetParamGlobal(HashId("Junk"), 0.5f);  // normal + reserve full
  SND_CHECK_EQ(fx.sys->Stop(p, 0.0f, nullptr), SND_OK);                          // coalesced bit
  SND_CHECK_EQ(fx.sys->SetParamGlobal(HashId("Vol"), 0.3f), SND_OK);
  SND_CHECK_EQ(fx.sys->SetParamGlobal(HashId("Vol"), 1.0f), SND_OK);  // same key: last value
  SND_CHECK_EQ(fx.sys->SetParamGlobal(HashId("J2"), 1.0f), SND_OK);
  SND_CHECK_EQ(fx.sys->SetParamGlobal(HashId("J3"), 1.0f), SND_OK);   // 4 keys: Junk, Vol, J2, J3
  fx.sys->SetParamGlobal(HashId("J4"), 1.0f);                         // 5th key: lost
  fx.Steps(6);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  SND_CHECK_EQ(fx.CountEnds(qh.value), 0u);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f * 0.1f, 1e-5f);
  snd_stats s1{};
  fx.sys->GetStats(&s1);
  SND_CHECK(s1.reserve_overflows > s0.reserve_overflows);
}

// C8: several registered threads record at the same time; every command arrives.
SND_TEST(IndepCmd, OtherThreadsRecordConcurrentlyAndEverythingArrives) {
  snd_config cfg = Config();
  cfg.max_real_voices = 64;
  cfg.max_playbacks = 256;
  Bank b;
  b.Event("Short", 2000.0f + 512.0f);  // 2 blocks of DC 1.0
  Fx fx(cfg);
  fx.Load("b", b.Build("B"));
  constexpr int kThreads = 4, kPer = 40;
  std::vector<std::vector<std::uint64_t>> handles(kThreads);
  std::vector<std::thread> ts;
  std::atomic<std::uint64_t> rendered{0};
  std::atomic<int> finished{0};
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      SND_REQUIRE_EQ(fx.sys->RegisterThread(), SND_OK);
      for (int i = 0; i < kPer; ++i) {
        const snd_playback_handle h = fx.sys->Play(HashId("Short"), NoEmitter(), nullptr);
        handles[t].push_back(h.value);
        if ((i % 10) == 9) fx.sys->Flush();
      }
      // Publish whatever is still held (all arenas may have been in flight;
      // see Defect_core_unregister_loses_unpublished_commands) before leaving.
      for (int k = 0; k < 3; ++k) {
        const std::uint64_t b0 = rendered.load();
        while (rendered.load() < b0 + 2u) std::this_thread::yield();
        fx.sys->Flush();
      }
      fx.sys->UnregisterThread();
      finished.fetch_add(1);
    });
  }
  // The audio side keeps running while the threads record.
  for (int i = 0; i < 5000 && finished.load() < kThreads; ++i) {
    fx.Step();
    rendered.fetch_add(1);
  }
  for (auto& th : ts) th.join();
  fx.Steps(20);
  std::uint32_t completed = 0;
  for (const auto& v : handles) {
    for (std::uint64_t h : v) {
      SND_CHECK(h != 0u);
      completed += fx.Ended(h, SND_END_COMPLETED) ? 1u : 0u;
      SND_CHECK_EQ(fx.CountEnds(h), 1u);
    }
  }
  SND_CHECK_EQ(completed, static_cast<std::uint32_t>(kThreads * kPer));
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
  if (completed != static_cast<std::uint32_t>(kThreads * kPer)) {
    std::uint32_t byReason[8] = {};
    for (const End& e : fx.ends) byReason[e.reason & 7u]++;
    std::printf("ends by reason: C%u S%u St%u R%u D%u E%u NL%u; starts %llu\n", byReason[1], byReason[2], byReason[3],
                byReason[4], byReason[5], byReason[6], byReason[7], static_cast<unsigned long long>(fx.kind.starts.load()));
    for (const End& e : fx.ends) if (e.reason != 1u) std::printf(" reason %u detail %u\n", e.reason, e.detail);
  }
}

// C9 / D-29: unregistered threads and invalid handles record nothing.
SND_TEST(IndepCmd, UnregisteredThreadAndInvalidHandles) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  std::thread t([&] {
    SND_CHECK_EQ(fx.sys->Play(HashId("E"), NoEmitter(), nullptr).value, 0u);
    SND_CHECK_EQ(fx.sys->SetParamGlobal(HashId("x"), 1.0f), SND_ERR_THREAD_NOT_REGISTERED);
    SND_CHECK_EQ(fx.sys->Flush(), SND_ERR_THREAD_NOT_REGISTERED);
  });
  t.join();
  SND_CHECK_EQ(fx.sys->Stop(snd_playback_handle{0}, 0.0f, nullptr), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(fx.sys->Pause(snd_playback_handle{0}, 0.0f), SND_ERR_INVALID_HANDLE);
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(2);
  // A stale generation of a live slot.
  const snd_playback_handle stale{SND_HANDLE_MAKE(SND_HANDLE_SLOT(p.value), SND_HANDLE_GENERATION(p.value) + 7u)};
  SND_CHECK_EQ(fx.sys->Stop(stale, 0.0f, nullptr), SND_ERR_INVALID_HANDLE);
  fx.Steps(3);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);
  SND_CHECK_EQ(fx.sys->GetPlaybackState(stale), SND_PLAYBACK_STATE_NONE);
}

// C11: CLOCK_SAMPLE start inside a block is sample accurate; DELAY_US counts
// from the start of the block in which the command is applied.
SND_TEST(IndepCmd, SampleAccurateClockAndDelayStarts) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const std::size_t base = fx.Total();
  const snd_when w = AtClock(base + 3 * 256 + 77);
  fx.sys->Play(HashId("E"), NoEmitter(), &w);
  fx.Steps(6);
  const std::size_t at = base + 3 * 256 + 77;
  SND_CHECK_EQ(fx.At(at - 1), 0.0f);
  SND_CHECK_NEAR(fx.At(at), kCenter * 0.5f, 1e-6f);
  SND_CHECK_NEAR(fx.At(at + 1), kCenter * 0.5f, 1e-6f);

  Fx g;
  g.Load("b", OneEvent("E", 500.0f));
  snd_when d{};
  d.kind = SND_WHEN_DELAY_US;
  d.value = 10000;  // 480 frames at 48 kHz
  g.sys->Play(HashId("E"), NoEmitter(), &d);
  const std::size_t applyBlockStart = g.Total();  // the next RenderBlock applies it
  g.Steps(5);
  SND_CHECK_EQ(g.At(applyBlockStart + 479), 0.0f);
  SND_CHECK_NEAR(g.At(applyBlockStart + 480), kCenter * 0.5f, 1e-6f);
}

// P1 / P3: states and the stop fade (length ~ fade seconds, monotone, starts at
// full level).
SND_TEST(IndepCmd, StopFadeShapeLengthAndStates) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_NONE);  // nothing published yet
  fx.Steps(3);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
  fx.sys->Stop(p, 0.1f, nullptr);
  const std::size_t s = fx.Total();
  fx.Step();
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_STOPPING);
  fx.Steps(30);
  SND_CHECK_NEAR(fx.At(s), kCenter * 0.5f, 0.02f);  // no jump at the fade start
  float prev = fx.At(s);
  bool monotone = true;
  std::size_t silentAt = 0;
  for (std::size_t f = s; f < fx.Total(); ++f) {
    if (fx.At(f) > prev + 1e-6f) monotone = false;
    prev = fx.At(f);
    if (silentAt == 0 && fx.At(f) == 0.0f) silentAt = f;
  }
  SND_CHECK(monotone);
  const double fadeSec = double(silentAt - s) / 48000.0;
  SND_CHECK_MSG(fadeSec > 0.09 && fadeSec < 0.115, "fade %f s", fadeSec);
  // Half way the level is about half (linear fade).
  SND_CHECK_NEAR(fx.At(s + 2400), kCenter * 0.25f, 0.03f);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  SND_CHECK_EQ(fx.CountEnds(p.value), 1u);
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_NONE);  // slot returned: stale
}

// P4 / P7: pause keeps the position (frames do not advance), resume continues.
SND_TEST(IndepCmd, PauseKeepsPositionAndResumeContinues) {
  Fx fx;
  fx.Load("b", OneEvent("C", 1000.0f));  // counter
  const snd_playback_handle p = fx.sys->Play(HashId("C"), NoEmitter(), nullptr);
  fx.Steps(5);
  SND_CHECK_EQ(fx.sys->Pause(p, 0.0f), SND_OK);
  fx.Steps(4);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PAUSED);
  snd_position a{}, b{};
  SND_CHECK_EQ(fx.sys->GetPosition(p, &a), SND_OK);
  fx.Steps(10);
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPosition(p, &b), SND_OK);
  SND_CHECK_EQ(a.frames, b.frames);
  SND_CHECK_EQ(fx.sys->ResumePlayback(p, 0.0f), SND_OK);
  fx.Steps(6);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
  snd_position c{};
  fx.sys->GetPosition(p, &c);
  SND_CHECK(c.frames > b.frames);
  SND_CHECK(c.frames <= b.frames + 6u * 256u);
  // Steady counter again; the source's playhead never jumped.
  const std::size_t f = fx.Total() - 100;
  SND_CHECK_NEAR(fx.At(f + 1) - fx.At(f), kCenter / 4096.0f, 1e-6f);
  SND_CHECK_EQ(fx.kind.discont.load(), 0u);
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
}

// P4: pause and resume in the same frame: latest wins, it keeps playing.
SND_TEST(IndepCmd, PauseThenResumeInOneFrameKeepsPlaying) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  fx.sys->Pause(p, 0.0f);
  fx.sys->ResumePlayback(p, 0.0f);
  fx.Steps(5);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK_NEAR(fx.LastMean(), kCenter * 0.5f, 1e-5f);
}

// P5: stop while paused ends STOPPED without an audible burst.
SND_TEST(IndepCmd, StopWhilePausedEndsSilently) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), nullptr);
  fx.Steps(3);
  fx.sys->Pause(p, 0.0f);
  fx.Steps(5);
  const std::size_t s = fx.Total();
  fx.sys->Stop(p, 0.5f, nullptr);
  fx.Steps(5);
  SND_CHECK_EQ(fx.MaxAbs(s, fx.Total()), 0.0f);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
}

// P1: a finite source completes exactly once; the frames add up.
SND_TEST(IndepCmd, FiniteSourceCompletesOnceWithAllFrames) {
  Fx fx;
  fx.Load("b", OneEvent("F", 2000.0f + 1000.0f));  // 1000 frames of DC 1.0
  const std::size_t s = fx.Total();
  const snd_playback_handle p = fx.sys->Play(HashId("F"), NoEmitter(), nullptr);
  fx.Steps(10);
  std::size_t n = 0;
  for (std::size_t f = s; f < fx.Total(); ++f) n += fx.At(f) != 0.0f ? 1u : 0u;
  SND_CHECK_EQ(n, 1000u);
  SND_CHECK(fx.Ended(p.value, SND_END_COMPLETED));
  SND_CHECK_EQ(fx.CountEnds(p.value), 1u);
}

// G6 (2.6): suspend stops the common clock and the sound; resume continues.
SND_TEST(IndepCmd, SuspendStopsTheClockAndResumeContinues) {
  Fx fx;
  fx.Load("b", OneEvent("C", 1000.0f));
  fx.sys->Play(HashId("C"), NoEmitter(), nullptr);
  fx.Steps(4);
  fx.sys->Suspend();
  fx.Step();
  fx.sys->Update();
  snd_stats a{}, b{};
  fx.sys->GetStats(&a);
  fx.Steps(5);
  SND_CHECK_EQ(fx.LastMean(), 0.0f);
  fx.sys->Update();
  fx.sys->GetStats(&b);
  SND_CHECK_EQ(a.clock_sample, b.clock_sample);
  fx.sys->Resume();
  fx.Steps(4);
  const std::size_t f = fx.Total() - 50;
  SND_CHECK_NEAR(fx.At(f + 1) - fx.At(f), kCenter / 4096.0f, 1e-6f);
  SND_CHECK_EQ(fx.kind.ViolationSum(), 0u);
}

// G5: Render(frames) in odd pieces gives the same samples as whole blocks.
SND_TEST(IndepCmd, RenderInOddPiecesEqualsBlocks) {
  Fx a, b;
  a.Load("b", OneEvent("C", 1000.0f));
  b.Load("b", OneEvent("C", 1000.0f));
  a.all.clear();
  b.all.clear();
  a.sys->Play(HashId("C"), NoEmitter(), nullptr);
  b.sys->Play(HashId("C"), NoEmitter(), nullptr);
  a.sys->Update();
  b.sys->Update();
  tAudioThread = true;
  for (int i = 0; i < 12; ++i) a.Render();
  std::vector<float> got;
  const std::uint32_t pieces[] = {1, 100, 255, 256, 257, 77, 500, 3};
  std::size_t done = 0;
  for (int k = 0; done < 12u * 256u; ++k) {
    std::uint32_t n = pieces[k % 8];
    if (done + n > 12u * 256u) n = static_cast<std::uint32_t>(12u * 256u - done);
    std::vector<float> buf(std::size_t{n} * b.Ch());
    b.sys->Render(buf.data(), n);
    got.insert(got.end(), buf.begin(), buf.end());
    done += n;
  }
  SND_REQUIRE_EQ(got.size(), a.all.size());
  SND_CHECK(std::memcmp(got.data(), a.all.data(), got.size() * sizeof(float)) == 0);
}


// 4.1: a future start is PENDING; a stop while pending ends it STOPPED
// without ever sounding. (Clocks inside / before the apply block:
// Defect_core_scheduled_start_in_apply_block_never_starts.)
SND_TEST(IndepCmd, PendingStartCanBeStopped) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  fx.Steps(4);
  const std::size_t s = fx.Total();
  const snd_when future = AtClock(s + 100 * 256);
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), &future);
  fx.Steps(3);
  fx.sys->Update();
  SND_CHECK_EQ(fx.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PENDING);
  fx.sys->Stop(p, 0.0f, nullptr);
  fx.Steps(110);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
  SND_CHECK_EQ(fx.MaxAbs(s, fx.Total()), 0.0f);
}

// D-21: a musical When without a valid transport acts as IMMEDIATE and posts a
// TRANSPORT_GONE warning; a Stop at a past clock stops now; P7: the position
// counts the frames played.
SND_TEST(IndepCmd, MusicalWhenWithoutTransportPastStopAndPosition) {
  Fx fx;
  fx.Load("b", OneEvent("E", 500.0f));
  snd_when bar{};
  bar.kind = SND_WHEN_NEXT_BAR;
  bar.transport = snd_transport_handle{SND_HANDLE_MAKE(3, 9)};  // never issued
  const snd_playback_handle p = fx.sys->Play(HashId("E"), NoEmitter(), &bar);
  const std::size_t s = fx.Total();
  fx.Steps(4);
  SND_CHECK_NEAR(fx.At(s + 1), kCenter * 0.5f, 1e-5f);
  bool warned = false;
  for (const Err& e : fx.errors) warned = warned || e.detail == SND_END_DETAIL_TRANSPORT_GONE;
  SND_CHECK(warned);
  fx.sys->Update();
  snd_position pos{};
  SND_CHECK_EQ(fx.sys->GetPosition(p, &pos), SND_OK);
  SND_CHECK_EQ(pos.frames, 4u * 256u);
  SND_CHECK_NEAR(pos.seconds, 4.0 * 256.0 / 48000.0, 1e-9);
  const snd_when past = AtClock(1);
  fx.sys->Stop(p, 0.0f, &past);
  fx.Steps(4);
  SND_CHECK(fx.Ended(p.value, SND_END_STOPPED));
}
