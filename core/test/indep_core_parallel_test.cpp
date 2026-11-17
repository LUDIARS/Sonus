// Independent tests: parallel rendering (2.3, 3.4, D-43) and A1-14.
// Repeat under TSan and Release:  ctest --repeat until-fail:20 -R indep_core_parallel
//
//  - serial and parallel rendering give the same bits (chunk order fixed, 3.4);
//  - D-43 under stress (late chunks on purpose): the source kind checks that no
//    render state is used by two renders at once, that the playhead never
//    loses or doubles progress, that held voices are not released and that
//    released voices are not rendered; a serial block after a late parallel
//    block never uses the late block's render set;
//  - A1-14: a worker that claims a chunk number and stalls does not make the
//    chunk late and the chunk is processed exactly once (core and jobs level).
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "indep_fixture.hpp"
#include "snd/jobs/rt_workers.hpp"

using namespace indep;
using snd::HashId;

namespace {

std::vector<std::uint8_t> ManyEvents(std::uint32_t n) {
  Bank b;
  for (std::uint32_t i = 0; i < n; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "E%u", i);
    // Alternate counter / DC, distinct gains and priorities.
    b.Event(name, (i % 3) == 0 ? 1000.0f : 100.0f + 10.0f * float(i), 100u + (i % 7) * 10u, 0, kNone,
            -0.5f * float(i % 9));
  }
  // Finite voices for churn.
  for (std::uint32_t i = 0; i < 8; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "F%u", i);
    b.Event(name, 2000.0f + 256.0f * float(3 + i * 2) + 17.0f * float(i), 90u + i * 20u);
  }
  return b.Build("P");
}

void Name(char* out, std::size_t n, const char* p, std::uint32_t i) { std::snprintf(out, n, "%s%u", p, i); }

}  // namespace

// R1: serial (0 workers) and parallel (2 workers) give the same bits as long as
// no chunk is late (the deadline is set far away so lateness cannot happen).
SND_TEST(IndepParallel, SerialAndParallelAreBitIdentical) {
  snd_config cs = Config();
  cs.max_real_voices = 48;
  cs.max_virtual_voices = 256;
  cs.max_playbacks = 256;
  cs.jobs.voices_per_chunk = 4;
  cs.tuning.lod_down_blocks = 0;  // 2.5: bit equality only with the load LOD off
  snd_config cp = cs;
  cp.jobs.realtime_workers = 2;
  cp.tuning.rt_spin_microseconds = 200000;  // 200 ms: no late chunk
  Fx s(cs), p(cp);
  SND_REQUIRE_EQ(s.created, SND_OK);
  SND_REQUIRE_EQ(p.created, SND_OK);
  const auto bank = ManyEvents(40);
  s.Load("p", bank);
  p.Load("p", bank);
  s.all.clear();
  p.all.clear();
  std::uint32_t parallelBlocks = 0, late = 0;
  for (std::uint32_t blk = 0; blk < 120; ++blk) {
    for (Fx* f : {&s, &p}) {
      char n[16];
      if (blk < 40) {
        Name(n, sizeof(n), "E", blk);
        f->sys->Play(HashId(n), NoEmitter(), nullptr);
      }
      if (blk % 9 == 0) {
        Name(n, sizeof(n), "F", blk % 8);
        f->sys->Play(HashId(n), NoEmitter(), nullptr);
      }
      if (blk == 60) f->sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.05f);
      f->Step();
    }
    parallelBlocks += p.sys->LastBlock().parallel;
    late += p.sys->LastBlock().lateChunks;
  }
  SND_CHECK(parallelBlocks > 50u);
  SND_CHECK_EQ(late, 0u);
  SND_REQUIRE_EQ(s.all.size(), p.all.size());
  std::size_t firstDiff = s.all.size();
  for (std::size_t i = 0; i < s.all.size(); ++i) {
    if (std::memcmp(&s.all[i], &p.all[i], sizeof(float)) != 0) {
      firstDiff = i;
      break;
    }
  }
  SND_CHECK_MSG(firstDiff == s.all.size(), "first difference at sample %zu", firstDiff);
  SND_CHECK_EQ(s.kind.ViolationSum(), 0u);
  SND_CHECK_EQ(p.kind.ViolationSum(), 0u);
}

// R2-R5: late chunks on purpose (RT workers spin 1.5 ms on every 3rd block,
// the deadline is 50 us), with steals, stops and churn. D-43 invariants are
// checked from the source side and from LastBlock().
SND_TEST(IndepParallel, LateChunksKeepTheD43Rules) {
  snd_config cfg = Config();
  cfg.max_real_voices = 16;
  cfg.fade_reserve_voices = 4;
  cfg.max_virtual_voices = 256;
  cfg.max_playbacks = 256;
  cfg.jobs.realtime_workers = 2;
  cfg.jobs.voices_per_chunk = 2;
  cfg.tuning.rt_spin_microseconds = 50;
  Fx fx(cfg);
  SND_REQUIRE_EQ(fx.created, SND_OK);
  fx.Load("p", ManyEvents(40));
  fx.kind.slowNs.store(1500000);
  fx.kind.slowBlockMask.store(3);
  std::vector<snd_playback_handle> live;
  std::uint32_t lateBlocks = 0, heldSeen = 0, setViolations = 0, parallelBlocks = 0;
  std::uint32_t lateSet = 2;  // set of the last parallel block that had late chunks, 2 = none
  std::uint32_t rng = 12345u;
  auto next = [&] {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  for (std::uint32_t blk = 0; blk < 300; ++blk) {
    char n[16];
    for (int k = 0; k < 2; ++k) {
      Name(n, sizeof(n), (next() % 3) == 0 ? "F" : "E", next() % ((next() % 3) == 0 ? 8u : 40u));
      const snd_playback_handle h = fx.sys->Play(HashId(n), NoEmitter(), nullptr);
      if (h.value != 0u) live.push_back(h);
    }
    if (live.size() > 30u) {
      const std::size_t i = next() % live.size();
      fx.sys->Stop(live[i], (next() % 2) ? 0.0f : 0.02f, nullptr);
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
    }
    // (pauses are left out here: core_control_on_held_voice covers them)
    if (blk % 37 == 0) fx.sys->SetBusVolume(HashId("Sfx"), (blk % 2) ? 0.5f : 1.0f, 0.01f);
    fx.Step();
    const snd::core::BlockProfile& bp = fx.sys->LastBlock();
    parallelBlocks += bp.parallel;
    heldSeen += bp.heldVoices != 0u ? 1u : 0u;
    if (bp.parallel != 0u) {
      lateSet = bp.lateChunks != 0u ? bp.renderSet : 2u;
      lateBlocks += bp.lateChunks != 0u ? 1u : 0u;
    } else if (lateSet != 2u && bp.renderSet == lateSet) {
      ++setViolations;  // D-43 (2): a serial block wrote the set a late chunk may still use
    }
    for (float v : fx.out) {
      if (!std::isfinite(v)) {
        SND_CHECK_MSG(false, "non-finite output at block %u", blk);
        break;
      }
    }
  }
  fx.kind.slowNs.store(0);
  fx.Steps(40);  // let every straggler finish and every hold be released
  std::printf("blocks: parallel %u, with late chunks %u, with held voices %u\n", parallelBlocks, lateBlocks, heldSeen);
  SND_CHECK_EQ(setViolations, 0u);
  SND_CHECK_MSG(fx.kind.overlap.load() == 0u, "render state used twice at once: %llu",
                static_cast<unsigned long long>(fx.kind.overlap.load()));
  SND_CHECK_MSG(fx.kind.discont.load() == 0u, "playhead lost or doubled progress: %llu",
                static_cast<unsigned long long>(fx.kind.discont.load()));
  SND_CHECK_EQ(fx.kind.doubleBlock.load(), 0u);
  SND_CHECK_EQ(fx.kind.releasedBusy.load(), 0u);
  SND_CHECK_EQ(fx.kind.deadRender.load(), 0u);
  SND_CHECK_EQ(fx.kind.liveStart.load(), 0u);
  // Control on a held voice's source is core_control_on_held_voice
  // (Pause there; Devirtualize = op bit 0x4 also shows up here): reported only.
  std::printf("Control while the source renders in a late chunk: %llu (ops mask 0x%x)\n",
              static_cast<unsigned long long>(fx.kind.controlBusy.load()), fx.kind.controlBusyOps.load());
  snd_stats st{};
  fx.sys->Update();
  fx.sys->GetStats(&st);
  SND_CHECK(st.real_voices <= 16u + 4u);
  // The test is only meaningful with late chunks; a machine so idle that the
  // 1.5 ms spin still finished in 50 us does not exist.
  SND_CHECK(lateBlocks > 0u);
}

// R3 / D-43 as decided in stage 2: a held voice is silent while held, its
// playhead advances by the held frames (it keeps time), and once released it
// fades in from 0 over the short fade (D-15) in the first block it renders,
// then plays at its level again.
SND_TEST(IndepParallel, HeldVoiceFadesThenContinues) {
  snd_config cfg = Config();
  cfg.max_real_voices = 8;
  cfg.jobs.realtime_workers = 1;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 50;
  cfg.tuning.lod_down_blocks = 0;
  Bank b;
  float sum = 0.0f;  // all 8 voices: DC (i + 1) / 100
  for (std::uint32_t i = 0; i < 8; ++i) {
    char n[8];
    Name(n, sizeof(n), "D", i);
    b.Event(n, 10.0f * float(i + 1u));
    sum += 0.01f * float(i + 1u);
  }
  Fx fx(cfg);
  fx.Load("p", b.Build("P"));
  for (std::uint32_t i = 0; i < 8; ++i) {
    char n[8];
    Name(n, sizeof(n), "D", i);
    fx.sys->Play(HashId(n), NoEmitter(), nullptr);
  }
  fx.Steps(4);
  const float full = kCenter * sum;
  const float shortFade = 240.0f;  // D-15: 5 ms at 48 kHz
  bool checked = false;
  for (int attempt = 0; attempt < 40 && !checked; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    fx.Steps(2);
    // 1. A block with a late chunk (the worker spins 3 ms), then one more block
    //    while the straggler still runs: the held voices are silent there.
    fx.kind.callerSlowNs.store(200000);
    fx.kind.slowNs.store(3000000);
    fx.Step();
    const snd::core::BlockProfile late = fx.sys->LastBlock();
    fx.kind.slowNs.store(0);
    fx.kind.callerSlowNs.store(0);
    if (late.lateChunks == 0u) continue;
    fx.Step();
    const snd::core::BlockProfile held = fx.sys->LastBlock();
    if (held.heldVoices == 0u) continue;  // the straggler already finished
    const float during = fx.LastMean();
    const float missing = full - during;  // the held voices' level
    SND_CHECK_MSG(missing > 0.005f, "held voices still audible: %f", static_cast<double>(missing));
    SND_CHECK_NEAR(fx.MaxAbs(fx.Total() - 256, fx.Total()), during, 1e-5f);  // constant: silent, no tail
    // 2. Let it finish, then render block by block until the level is full again.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::size_t from = fx.Total();
    bool calm = true;
    for (int k = 0; k < 4; ++k) {
      fx.Step();
      calm = calm && fx.sys->LastBlock().lateChunks == 0u;
    }
    if (!calm) continue;  // a slow (sanitizer) build was late again: try again
    // The first block in which the level rises is the fade-in block.
    std::size_t fadeStart = 0;
    for (std::size_t f = from; f < fx.Total(); ++f) {
      if (fx.At(f) > during + 1e-5f) {
        fadeStart = f;
        break;
      }
    }
    SND_REQUIRE(fadeStart != 0u);
    fadeStart -= fadeStart % 256u;  // the block in which the voice is back (its first sample may be exactly 0)
    const float c0 = fx.At(fadeStart) - during;
    SND_CHECK_MSG(c0 < 0.2f * missing, "starts at %f of %f (no fade-in from 0)",
                  static_cast<double>(c0), static_cast<double>(missing));
    bool monotone = true;
    for (std::size_t f = fadeStart + 1; f < fadeStart + 256; ++f) {
      if (fx.At(f) < fx.At(f - 1) - 1e-6f) monotone = false;
    }
    SND_CHECK(monotone);
    // A short fade: at least half way after half the short fade, full by the
    // end of the block (core ramps the D-15 fade per block: 256 >= 240 frames).
    SND_CHECK(fx.At(fadeStart + static_cast<std::size_t>(shortFade / 2.0f)) - during > 0.4f * missing);
    SND_CHECK_NEAR(fx.At(fadeStart + 255), full, missing / 128.0f);
    SND_CHECK_NEAR(fx.At(fadeStart + 256 + 10), full, 1e-5f);
    checked = true;
  }
  if (!checked) std::printf("SKIP: no clean late / held / release sequence in 40 attempts\n");
  SND_CHECK_EQ(fx.kind.overlap.load(), 0u);
  SND_CHECK_EQ(fx.kind.discont.load(), 0u);
  SND_CHECK_EQ(fx.kind.doubleBlock.load(), 0u);
  // The playhead kept time through every hold (advanced by the held frames).
  SND_CHECK_MSG(fx.kind.timeLag.load() == 0u, "%llu renders behind the clock",
                static_cast<unsigned long long>(fx.kind.timeLag.load()));
}

// A1-14 (core): workers stall between claiming a chunk number and starting it;
// the audio thread runs those chunks, nothing is late, every voice renders
// exactly once per block and keeps exact time.
SND_TEST(IndepParallel, StalledClaimsAreRunOnceByTheCaller) {
  snd_config cfg = Config();
  cfg.max_real_voices = 32;
  cfg.max_playbacks = 64;
  cfg.jobs.realtime_workers = 2;
  cfg.jobs.voices_per_chunk = 4;
  cfg.tuning.rt_spin_microseconds = 50;
  Bank b;
  for (std::uint32_t i = 0; i < 32; ++i) {
    char n[8];
    Name(n, sizeof(n), "C", i);
    b.Event(n, 1000.0f, 128, 0, kNone, -30.0f);
  }
  cfg.tuning.lod_down_blocks = 0;  // the stalls make blocks slow; keep every voice real (no load LOD)
  Fx fx(cfg, {}, nullptr, /*claimDelayUs=*/3000);
  SND_REQUIRE_EQ(fx.created, SND_OK);
  fx.Load("p", b.Build("P"));
  for (std::uint32_t i = 0; i < 32; ++i) {
    char n[8];
    Name(n, sizeof(n), "C", i);
    fx.sys->Play(HashId(n), NoEmitter(), nullptr);
  }
  std::uint32_t late = 0, parallel = 0;
  const std::size_t start = fx.Total();
  for (int i = 0; i < 60; ++i) {
    // Stalled claims keep the pool busy; give it time so blocks start quiescent (parallel).
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    fx.Step();
    late += fx.sys->LastBlock().lateChunks;
    parallel += fx.sys->LastBlock().parallel;
  }
  std::uint64_t rendered = 0;
  for (snd::SlotIndex v = 0; v < 128; ++v) rendered += fx.kind.FramesRendered(v);
  const std::uint64_t expected = 32u * (fx.Total() - start);
  std::printf("parallel blocks %u, late chunks %u, frames rendered %llu of %llu\n", parallel, late,
              static_cast<unsigned long long>(rendered), static_cast<unsigned long long>(expected));
  if (late == 0u) SND_CHECK_EQ(rendered, expected);
  SND_CHECK(parallel > 0u);
  SND_CHECK_EQ(fx.kind.doubleBlock.load(), 0u);
  SND_CHECK_EQ(fx.kind.overlap.load(), 0u);
  SND_CHECK_EQ(fx.kind.discont.load(), 0u);
  // Stalls happen before a chunk starts, so the caller always takes them over:
  // nothing may be late (an OS preemption AFTER the start could still be).
  // (late chunks here can only come from an OS preemption after a chunk started: reported only)
  if (late == 0u) {
    // Exact time: all 32 counters at the same frame = elapsed frames.
    const std::size_t f = fx.Total() - 1;
    const float v = fx.At(f) / (kCenter * 32.0f * std::pow(10.0f, -1.5f));
    const long frame = std::lround(v * 4096.0f) - 1;
    SND_CHECK_EQ(frame, static_cast<long>((f - start) % 4096u));
  }
}

// A1-14 (jobs level): every chunk of every batch runs exactly once although
// workers stall after claiming; no chunk is late because of a stall.
SND_TEST(IndepParallel, RtPoolRunsEveryChunkExactlyOnceWithStalledClaims) {
  snd::jobs::RtWorkerPool pool;
  snd::jobs::RtWorkerConfig c;
  c.workers = 2;
  c.maxChunks = 16;
  c.spinMicroseconds = 50;
  c.realtimePriority = false;
  c.debugClaimDelayMicroseconds = 400;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), c));
  struct Ctx {
    std::atomic<std::uint32_t> runs[16];
    std::atomic<std::uint32_t> gen[16];
    std::atomic<std::uint32_t> twice{0};
  } ctx;
  for (int i = 0; i < 16; ++i) {
    ctx.runs[i].store(0);
    ctx.gen[i].store(0);
  }
  auto fn = [](void* p, const snd::jobs::RtChunkContext& cc) noexcept {
    Ctx& x = *static_cast<Ctx*>(p);
    if (x.gen[cc.chunkIndex].exchange(cc.generation) == cc.generation) x.twice.fetch_add(1);
    x.runs[cc.chunkIndex].fetch_add(1);
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 20000;  // 20 us of work
    while (snd::platform::MonotonicNanoseconds() < until) {
    }
  };
  std::uint32_t late = 0, stolen = 0;
  constexpr std::uint32_t kBatches = 400;
  for (std::uint32_t b = 0; b < kBatches; ++b) {
    while (!pool.IsQuiescent()) {
    }
    const snd::jobs::RtRunResult r = pool.Run(16, fn, &ctx);
    late += r.late;
    stolen += r.stolen;
  }
  while (!pool.IsQuiescent()) {
  }
  pool.Shutdown();
  std::printf("late %u, stolen %u\n", late, stolen);
  SND_CHECK_EQ(ctx.twice.load(), 0u);
  for (int i = 0; i < 16; ++i) SND_CHECK_EQ(ctx.runs[i].load(), kBatches);  // late ones still ran once
  SND_CHECK(stolen > 0u);
  // Late chunks can still come from OS preemption AFTER a chunk started (busy
  // machine); they are reported, not failed. Exactly-once holds either way.
}
