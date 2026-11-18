// core: parallel rendering and late chunks (2.3, D-43). The test source's
// gate (test_sources.hpp) makes chunks miss the deadline without relying on
// timing: while it is closed, the audio thread waits in its own chunk of a
// parallel block until an RT worker has entered a chunk of the same block, and
// that worker stays in its chunk until the test opens the gate. So every
// parallel block rendered with the gate closed has a late chunk, and every
// block after it stays serial (the pool is not quiescent) until the gate is
// opened, whatever the load. Checks:
//   - a block renders in parallel only when the pool was quiescent at its start;
//   - a block that is not quiescent uses the other render set than the late
//     parallel block (at most one set is in use by a late chunk);
//   - voices of late chunks are held (not rendered, not freed) and resume
//     with their state once the pool is quiescent; the level comes back.
// Run under TSan (repeated) to catch races between late chunks and the next blocks.
#include <chrono>
#include <cmath>
#include <thread>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
// Renders until a block has no late chunk and no held voice, then two more blocks
// (a released voice plays again from the block after its release, D-43).
// Returns 2 after three calm blocks in a row (the last one renders every voice),
// 1 if at least one block had no held voice (holds are released), 0 otherwise.
int WaitCalm(Harness& h) {
  // Ordinary blocks can still be late now and then on a loaded machine (non-RT test
  // threads, sanitizers, other builds): wait for three calm blocks in a row.
  std::uint32_t calm = 0;
  bool released = false;
  for (int i = 0; i < 2000 && calm < 3u; ++i) {
    h.Step();
    const snd::core::BlockProfile& p = h.sys->LastBlock();
    released = released || p.heldVoices == 0u;
    calm = (p.heldVoices == 0u && p.lateChunks == 0u) ? calm + 1u : 0u;
    if (calm == 0u) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // let the late chunk finish
  }
  return calm >= 3u ? 2 : (released ? 1 : 0);
}
}  // namespace

SND_TEST(CoreParallel, LateChunksHoldVoicesAndAlternateSets) {
  snd_config cfg = SmallConfig();
  cfg.jobs.realtime_workers = 2;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 20000;  // ordinary blocks are not late (the gated ones always are)
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(4800000, 0.1f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  for (int i = 0; i < 6; ++i) h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  const float steady = 6 * 0.1f * 0.70710678f;
  if (WaitCalm(h) == 2) SND_CHECK_NEAR(h.out[100 * 2], steady, 1e-4f);
  std::uint32_t lateBlocks = 0, heldBlocks = 0, serialBlocks = 0;
  std::uint32_t lateSet = 2;  // set of the last parallel block that had late chunks
  std::uint32_t parallelBlocks = 0, parallelLate = 0, gatedSerial = 0, waitSteps = 0;
  // One rendered block of the slow phase: the D-43 checks and the counts.
  auto block = [&]() {
    h.Step();
    const snd::core::BlockProfile& p = h.sys->LastBlock();
    for (float v : h.out) SND_CHECK(std::isfinite(v) && std::fabs(v) <= steady + 1e-3f);
    if (p.parallel == 0u && lateSet != 2u) {
      ++serialBlocks;
      SND_CHECK(p.renderSet != lateSet);  // D-43: never the set a late chunk may still write
    }
    if (p.parallel != 0u) {
      ++parallelBlocks;
      if (p.lateChunks != 0u) ++parallelLate;
      lateSet = p.lateChunks != 0u ? p.renderSet : 2u;
    }
    if (p.lateChunks != 0u) ++lateBlocks;
    if (p.heldVoices != 0u) ++heldBlocks;
    return p;
  };
  for (int cycle = 0; cycle < 3; ++cycle) {
    h.sample.CloseGate();
    // Blocks stay serial while the last late chunk runs (or until the pool is
    // quiescent again after the gate opened): render until a parallel one.
    bool gotParallel = false;
    for (int i = 0; i < 20000 && !gotParallel; ++i) {
      gotParallel = block().parallel != 0u;
      if (!gotParallel) {
        ++waitSteps;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // let the opened chunk return
      }
    }
    SND_CHECK_MSG(gotParallel, "cycle %d: the pool never became quiescent", cycle);
    if (!gotParallel) break;
    SND_CHECK(h.sys->LastBlock().lateChunks != 0u);   // the gated worker's chunk missed the deadline
    SND_CHECK(h.sys->LastBlock().heldVoices != 0u);   // ... and its voices are held
    // The gated chunk still runs: the next blocks are serial, in the other set, voices held.
    for (int k = 0; k < 3; ++k) {
      const snd::core::BlockProfile& p = block();
      SND_CHECK_EQ(p.parallel, 0u);
      SND_CHECK(p.heldVoices != 0u);
      if (p.parallel == 0u) ++gatedSerial;
    }
    h.sample.OpenGate();
  }
  // The gate is open: the last late chunk returns; wait until every hold is released.
  const int calm = WaitCalm(h);
  snd::test::Note("late blocks %u, blocks with held voices %u, serial blocks while busy %u (%u waiting)", lateBlocks,
                  heldBlocks, serialBlocks, waitSteps);
  SND_CHECK_EQ(h.sample.gateTimeouts.load(), 0u);     // a worker joined every gated parallel block
  SND_CHECK_EQ(parallelLate, parallelBlocks);           // ... so each of them had a late chunk
  SND_CHECK_EQ(lateBlocks, 3u);
  SND_CHECK_EQ(gatedSerial, 9u);                       // the blocks after a late one were serial ...
  SND_CHECK(serialBlocks >= gatedSerial);              // ... and used the other set
  SND_CHECK(heldBlocks > 0u);
  SND_CHECK(calm >= 1);
  if (calm < 2) {
    snd::test::Note("machine too busy for three calm blocks in a row: level check skipped");
    return;
  }
  // Every hold is released and every voice plays again at its level.
  SND_CHECK_EQ(h.sys->LastBlock().heldVoices, 0u);
  SND_CHECK_NEAR(h.out[100 * 2], steady, 1e-4f);
  h.sys->Update();
  snd_stats st{};
  h.sys->GetStats(&st);
  SND_CHECK(st.late_chunks > 0u);
  SND_CHECK_EQ(st.playbacks, 6u);
}

SND_TEST(CoreParallel, StalledClaimDoesNotMakeChunksLate) {
  // A1-14 through core: a worker that claims a chunk and stalls; the audio thread runs it.
  snd_config cfg = SmallConfig();
  cfg.jobs.realtime_workers = 1;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 50;
  Harness h(cfg, {}, nullptr, 3000);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(4800000, 0.1f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  for (int i = 0; i < 4; ++i) h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  std::uint32_t late = 0;
  for (int i = 0; i < 30; ++i) {
    h.Step();
    late += h.sys->LastBlock().lateChunks;
  }
  // Without A1-14 nearly every block would lose the chunk the stalled worker claimed;
  // a stray late chunk on a loaded machine (sanitizers) is tolerated.
  SND_CHECK(late <= 3u);
}
