// core: parallel rendering and late chunks (2.3, D-43). A source made slow on
// purpose makes chunks miss the deadline; checks:
//   - a block renders in parallel only when the pool was quiescent at its start;
//   - a block that is not quiescent uses the other render set than the late
//     parallel block (at most one set is in use by a late chunk);
//   - voices of late chunks are held (not rendered, not freed) and resume
//     with their state once the pool is quiescent; the level comes back.
// Run under TSan (repeated) to catch races between late chunks and the next blocks.
#include <cmath>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

SND_TEST(CoreParallel, LateChunksHoldVoicesAndAlternateSets) {
  snd_config cfg = SmallConfig();
  cfg.jobs.realtime_workers = 2;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 20;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(4800000, 0.1f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  for (int i = 0; i < 6; ++i) h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  const float steady = 6 * 0.1f * 0.70710678f;
  SND_CHECK_NEAR(h.out[100 * 2], steady, 1e-4f);
  std::uint32_t lateBlocks = 0, heldBlocks = 0, serialBlocks = 0;
  std::uint32_t lateSet = 2;  // set of the last parallel block that had late chunks
  h.sample.slowNs.store(2000000);  // 2 ms per batch: far past the 20 us spin
  for (int i = 0; i < 40; ++i) {
    h.Step();
    const snd::core::BlockProfile& p = h.sys->LastBlock();
    for (float v : h.out) SND_CHECK(std::isfinite(v) && std::fabs(v) <= steady + 1e-3f);
    if (p.parallel == 0u && lateSet != 2u) {
      ++serialBlocks;
      SND_CHECK(p.renderSet != lateSet);  // D-43: never the set a late chunk may still write
    }
    if (p.parallel != 0u) lateSet = p.lateChunks != 0u ? p.renderSet : 2u;
    if (p.lateChunks != 0u) ++lateBlocks;
    if (p.heldVoices != 0u) ++heldBlocks;
  }
  h.sample.slowNs.store(0);
  h.Steps(30);
  snd::test::Note("late blocks %u, blocks with held voices %u, serial blocks while busy %u", lateBlocks, heldBlocks,
                  serialBlocks);
  SND_CHECK(lateBlocks > 0u);
  SND_CHECK(heldBlocks > 0u);
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
  SND_CHECK_EQ(late, 0u);
  SND_CHECK_NEAR(h.out[100 * 2], 4 * 0.1f * 0.70710678f, 1e-4f);
}
