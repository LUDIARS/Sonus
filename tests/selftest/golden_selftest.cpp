// Tests of the golden audio comparison.
#include <cmath>
#include <cstdio>
#include <string>

#include "snd/test/golden.hpp"
#include "snd/test/test.hpp"

using snd::test::AudioBuffer;
using snd::test::GoldenMode;
using snd::test::GoldenTolerance;

namespace {

// Deterministic test signal: two channels of sine at 440 / 660 Hz.
AudioBuffer MakeSine(std::uint64_t frames) {
  AudioBuffer b(2, frames, 48000);
  for (std::uint64_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    b.Channel(0)[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
    b.Channel(1)[i] = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979323846 * 660.0 * t));
  }
  return b;
}

}  // namespace

SND_TEST(Golden, IdenticalBuffersMatch) {
  AudioBuffer a = MakeSine(512);
  snd::test::GoldenDiff d = snd::test::DiffAudio(a, a, GoldenTolerance{});
  SND_CHECK(d.passed);
  SND_CHECK_EQ(d.mismatches, 0u);
  SND_CHECK_EQ(d.maxAbsDiff, 0.0);
}

SND_TEST(Golden, ToleranceIsRespected) {
  AudioBuffer ref = MakeSine(512);
  AudioBuffer act = ref;
  act.Channel(1)[100] += 1e-4f;
  GoldenTolerance loose;
  loose.maxAbs = 1e-3f;
  SND_CHECK(snd::test::DiffAudio(ref, act, loose).passed);

  GoldenTolerance strict;
  strict.maxAbs = 1e-6f;
  snd::test::GoldenDiff d = snd::test::DiffAudio(ref, act, strict);
  SND_CHECK(!d.passed);
  SND_CHECK_EQ(d.mismatches, 1u);
  SND_CHECK_EQ(d.maxChannel, 1u);
  SND_CHECK_EQ(d.maxFrame, 100u);
  SND_CHECK_EQ(d.firstFrame, 100u);
  // Summary names the location of the worst sample.
  SND_CHECK_MSG(d.summary.find("ch1 frame 100") != std::string::npos, "%s", d.summary.c_str());

  GoldenTolerance allowOne = strict;
  allowOne.maxMismatches = 1;
  SND_CHECK(snd::test::DiffAudio(ref, act, allowOne).passed);
}

SND_TEST(Golden, RmsToleranceAndNaN) {
  AudioBuffer ref = MakeSine(256);
  AudioBuffer act = ref;
  for (float& s : act.samples) s += 0.01f;
  GoldenTolerance tol;
  tol.maxAbs = 0.1f;
  tol.maxRms = 0.001f;
  snd::test::GoldenDiff d = snd::test::DiffAudio(ref, act, tol);
  SND_CHECK(!d.passed);
  SND_CHECK_NEAR(d.rmsDiff, 0.01, 1e-4);

  AudioBuffer nan = ref;
  nan.Channel(0)[7] = std::nanf("");
  snd::test::GoldenDiff dn = snd::test::DiffAudio(ref, nan, GoldenTolerance{});
  SND_CHECK(!dn.passed);
  SND_CHECK_EQ(dn.maxFrame, 7u);
}

SND_TEST(Golden, ShapeMismatchIsReported) {
  AudioBuffer a = MakeSine(256);
  AudioBuffer b = MakeSine(255);
  snd::test::GoldenDiff d = snd::test::DiffAudio(a, b, GoldenTolerance{});
  SND_CHECK(d.shapeMismatch);
  SND_CHECK(!d.passed);
  SND_CHECK(d.summary.find("shape mismatch") != std::string::npos);
}

SND_TEST(Golden, InterleavedConversion) {
  const float inter[6] = {1, 2, 3, 4, 5, 6};
  AudioBuffer b = AudioBuffer::FromInterleaved(inter, 2, 3);
  SND_CHECK_EQ(b.Channel(0)[2], 5.0f);
  SND_CHECK_EQ(b.Channel(1)[0], 2.0f);
}

// Update -> compare round trip in a scratch directory of the build tree.
SND_TEST(Golden, UpdateThenCompareRoundTrip) {
  const char* dir = SND_GOLDEN_SCRATCH_DIR;
  AudioBuffer a = MakeSine(300);
  std::remove((std::string(dir) + "/roundtrip.sndg").c_str());

  snd::test::GoldenResult missing = snd::test::CompareGolden(dir, "roundtrip", a, GoldenTolerance{},
                                                             GoldenMode::Compare);
  SND_CHECK(!missing.passed);
  SND_CHECK(missing.missing);
  SND_CHECK(missing.summary.find("--update-golden") != std::string::npos);

  snd::test::GoldenResult up =
      snd::test::CompareGolden(dir, "roundtrip", a, GoldenTolerance{}, GoldenMode::Update);
  SND_REQUIRE(up.updated);
  snd::test::GoldenResult same =
      snd::test::CompareGolden(dir, "roundtrip", a, GoldenTolerance{}, GoldenMode::Compare);
  SND_CHECK_MSG(same.passed, "%s", same.summary.c_str());

  AudioBuffer changed = a;
  changed.Channel(0)[10] = 0.9f;
  snd::test::GoldenResult diff =
      snd::test::CompareGolden(dir, "roundtrip", changed, GoldenTolerance{}, GoldenMode::Compare);
  SND_CHECK(!diff.passed);
  SND_CHECK(diff.summary.find("ch0 frame 10") != std::string::npos);
}

// Checked-in reference (tests/selftest/golden/sine_440_660.sndg). Regenerate with
// `testkit_selftest --update-golden --filter Golden.CommittedReference`.
SND_TEST(Golden, CommittedReference) {
  GoldenTolerance tol;
  tol.maxAbs = 1e-6f;
  SND_CHECK_GOLDEN("sine_440_660", MakeSine(480), tol);
}
