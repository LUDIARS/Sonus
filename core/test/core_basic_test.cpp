// core: creation, silence, constant level through the bus graph (2D pan law),
// end notification, queries.
#include <cmath>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

SND_TEST(CoreBasic, CreatesAndRendersSilence) {
  Harness h;
  SND_REQUIRE_EQ(h.created, SND_OK);
  h.Steps(4);
  for (float v : h.all) SND_CHECK_EQ(v, 0.0f);
  h.sys->Update();  // queries see the state as of the latest Update (2.1)
  snd_stats st{};
  SND_CHECK_EQ(h.sys->GetStats(&st), SND_OK);
  SND_CHECK_EQ(st.block_index, 4u);
}

SND_TEST(CoreBasic, RejectsBadConfig) {
  snd_config c = SmallConfig();
  c.block_size = 100;
  snd::core::CoreDesc d;
  d.config = &c;
  d.allocator = &snd::base::DefaultAllocator();
  snd::core::System* s = nullptr;
  SND_CHECK_EQ(snd::core::System::Create(d, &s), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(s == nullptr);
  c = SmallConfig();
  c.max_pitch_ratio = 0.5f;
  SND_CHECK_EQ(snd::core::System::Create(d, &s), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(CoreBasic, ConstantMonoCenterIsMinus3dB) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(48000, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  const snd_bank_handle bank = h.Load("b.sndb", b.Build(HashId("B")));
  SND_REQUIRE_EQ(h.sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(h.ends.loaded, 1u);
  h.Clear();
  const snd_playback_handle p = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  h.Steps(8);
  // Mono -> stereo plain downmix 0.7071 (dsp pan.hpp), no bus gain.
  const float expect = 0.5f * 0.70710678f;
  bool any = false;
  for (std::size_t f = h.Frames() * 2; f < h.TotalFrames(); ++f) {
    SND_CHECK_NEAR(h.At(f, 0), expect, 1e-4f);
    SND_CHECK_NEAR(h.At(f, 1), expect, 1e-4f);
    any = true;
  }
  SND_CHECK(any);
  SND_CHECK_EQ(h.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
  snd_position pos{};
  SND_CHECK_EQ(h.sys->GetPosition(p, &pos), SND_OK);
  SND_CHECK(pos.frames > 0u);
  snd_meter m{};
  SND_CHECK_EQ(h.sys->GetMeter(0, &m), SND_OK);
  SND_CHECK_NEAR(m.peak[0], expect, 1e-3f);
}

SND_TEST(CoreBasic, EndsWhenTheSampleEndsExactlyOnce) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("short"), 1, 48000, Constant(1000, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("S", 0));
  h.Load("b.sndb", b.Build(HashId("B")));
  const snd_playback_handle p = h.sys->Play(HashId("S"), snd_emitter_handle{0}, nullptr);
  h.Steps(20);
  SND_CHECK_EQ(h.ends.CountOf(p), 1u);
  SND_CHECK(h.ends.Has(p, SND_END_COMPLETED));
  SND_CHECK_EQ(h.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_NONE);
}

SND_TEST(CoreBasic, UnknownEventEndsNotLoaded) {
  Harness h;
  const snd_playback_handle p = h.sys->Play(HashId("nope"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  h.Steps(3);
  SND_CHECK(h.ends.Has(p, SND_END_NOT_LOADED));
}
