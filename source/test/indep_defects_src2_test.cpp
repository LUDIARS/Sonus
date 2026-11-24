#include "indep_src2_util.hpp"
#include "snd/test/test.hpp"

using namespace indep2;

SND_TEST(IndepSrc2, Defect_GranularSeekPublishesAnIntegerPlayhead) {
  const std::vector<float> pcm{0.5f};
  auto a = Pcm(pcm, 48000);
  src::GranularParams p{&a, 48000, 0, 0, 0, src::GrainWindow::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice v, fresh;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Advance(v, 1, 0.5f);
  SND_REQUIRE_EQ(v.ph.fraction, 0x80000000u);
  d.Control(v, src::ControlOp::Seek, 17);
  SND_REQUIRE(d.Start(fresh, &p, 1, 0, 17));
  SND_CHECK(Item(d, v, 1) == Item(d, fresh, 1));
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{18});
  SND_CHECK_EQ(v.ph.fraction, fresh.ph.fraction);
}

SND_TEST(IndepSrc2, Defect_GranularLongVirtualDurationIsPartitionIndependent) {
  const std::vector<float> pcm{0.5f};
  auto a = Pcm(pcm, 48000);
  src::GranularParams p{&a, 48000, 0, 0, 0, src::GrainWindow::Rectangle};
  Driver d(snd::SourceKind::Granular, {});
  Driver::Voice once, split;
  SND_REQUIRE(d.Start(once, &p, 1));
  SND_REQUIRE(d.Start(split, &p, 1));
  constexpr float ratio = 1.f + 0x1p-23f;
  constexpr std::uint32_t n = UINT32_MAX;
  const std::uint64_t frame = std::uint64_t{n} + (n >> 23);
  const std::uint32_t fraction = (n & 0x7fffff) << 9;
  d.Advance(once, n, ratio);
  // All the pieces have products representable in double. Their exact sum is n * ratio.
  for (unsigned i = 0; i < 1023; ++i) d.Advance(split, 1u << 22, ratio);
  d.Advance(split, (1u << 22) - 1, ratio);
  SND_REQUIRE_EQ(split.ph.frame, frame);
  SND_REQUIRE_EQ(split.ph.fraction, fraction);
  SND_CHECK_EQ(once.ph.frame, frame);
  SND_CHECK_EQ(once.ph.fraction, fraction);
}

SND_TEST(IndepSrc2, Defect_OscillatorStartsAtFirstEffectiveFrequency) {
  // Pulse(0.25) has a nonzero PolyBLEP value at its start, so premature sound is visible.
  src::OscillatorParams p{};
  p.waveform = 4;
  p.duty = 0.25f;
  p.frequencyHz = 375;
  for (bool virtualStart : {false, true}) {
    for (float first : {0.f, -1.f, float(NAN), float(INFINITY)}) {
      Driver d(snd::SourceKind::Oscillator, {});
      Driver::Voice actual, full;
      SND_REQUIRE(d.Start(actual, &p, 1));
      SND_REQUIRE(d.Start(full, &p, 1));
      src::RenderItem it{};
      it.pitch = {1, 1};
      Item(d, actual, 0, it);  // an empty block cannot choose the starting gain
      d.Advance(actual, 0, 1);
      if (virtualStart) {
        d.Advance(actual, 7, first);
      } else {
        it.pitch = {first, first};
        SND_CHECK(Item(d, actual, 7, it) == std::vector<float>(7, 0));
      }
      it.pitch = {1, 1};
      auto voice = Item(d, actual, 260, it);
      const auto reference = Item(d, full, 260, it);
      for (unsigned i = 0; i < voice.size(); ++i) {
        const double gain = std::min(1.0, (i + 1) / 240.0);
        SND_REQUIRE(std::abs(voice[i] - reference[i] * gain) <= 1.2e-7);
      }
    }
  }
}
