#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;

SND_TEST(SourceGranular, ComplementaryHannWindowsAndVirtualContinuation) {
  std::vector<float> pcm(256, 0.75f);
  src::SourceAsset asset{};
  asset.codec.codec = SND_CODEC_PCM_F32;
  asset.codec.channels = 1;
  asset.codec.sample_rate = 1000;
  asset.codec.total_frames = pcm.size();
  asset.data = reinterpret_cast<const std::uint8_t*>(pcm.data());
  asset.dataBytes = pcm.size() * sizeof(float);
  src::GranularParams p{&asset, 125, 16, 0, 0};
  Driver::Opt options;
  options.sampleRate = 1000;
  Driver d(snd::SourceKind::Granular, options);
  SND_REQUIRE(d.Ok());
  Driver::Voice a, b;
  SND_REQUIRE(d.Start(a, &p, 2, 31));
  SND_REQUIRE(d.Start(b, &p, 2, 31));
  d.Render(a, 512, {}, 29);
  for (unsigned i = 8; i < 512; ++i) SND_CHECK_NEAR(a.out[0][i], 0.75, 1e-6);
  d.Advance(b, 405);
  d.Render(b, 107);
  for (unsigned i = 0; i < 107; ++i) SND_CHECK_EQ(b.out[1][i], a.out[0][405 + i]);
}

SND_TEST(SourceGranular, FractionalDensityControlsSpectrallyIsolatedBirths) {
  const float dc = 1;
  src::SourceAsset asset{};
  asset.codec.codec = SND_CODEC_PCM_F32;
  asset.codec.channels = 1;
  asset.codec.sample_rate = 48000;
  asset.codec.total_frames = 1;
  asset.data = reinterpret_cast<const std::uint8_t*>(&dc);
  asset.dataBytes = sizeof(dc);
  src::GranularParams p{&asset, 73.5f, 0, 1, 0};
  p.window = src::GrainWindow::Rectangle;
  Driver d(snd::SourceKind::Granular, {});
  SND_REQUIRE(d.Ok());
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Render(v, 48000);
  unsigned births = 0;
  for (float x : v.out[0]) births += x == 1;
  SND_CHECK_EQ(births, 74u);
}

SND_TEST(SourceGranular, SubQ32VirtualPitchCarriesAcrossDifferentRatios) {
  float pcm = 0;
  src::SourceAsset a{};
  a.codec.codec = SND_CODEC_PCM_F32;
  a.codec.channels = 1;
  a.codec.sample_rate = 48000;
  a.codec.total_frames = 1;
  a.data = reinterpret_cast<const std::uint8_t*>(&pcm);
  a.dataBytes = sizeof(pcm);
  src::GranularParams p{&a, 0, 0, 0, 0};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Advance(v, 1, 0x1p-33f);
  d.Advance(v, UINT32_MAX, 32.f);
  d.Advance(v, 2, 0x1p-34f);
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{UINT32_MAX} * 32);
  SND_CHECK_EQ(v.ph.fraction, 1u);
}
