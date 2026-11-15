// Voice filter, biquads and meters.
#include <cmath>
#include <memory>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/meter.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

namespace {

constexpr double kSr = 48000.0;

void FillSine(Audio& a, double f, float amp, std::uint64_t start = 0) {
  for (std::uint32_t c = 0; c < a.channels(); ++c)
    for (std::uint32_t i = 0; i < a.frames(); ++i)
      a.ch(c)[i] = amp * static_cast<float>(std::sin(2.0 * dsptest::kPi * f * static_cast<double>(start + i) / kSr));
}

// Steady-state gain (dB) of the voice filter for a sine at f.
double VoiceFilterGain(const snd::FilterTarget& t, double f) {
  snd::FilterState st;
  VoiceFilterInit(st, t);
  Audio a(1, 256);
  std::vector<float> out;
  for (int b = 0; b < 40; ++b) {
    FillSine(a, f, 0.5f, static_cast<std::uint64_t>(b) * 256u);
    {
      SND_RT_SCOPE();
      VoiceFilterProcess(st, a.p(), 1, 256, t, 48000);
    }
    out.insert(out.end(), a.ch(0), a.ch(0) + 256);
  }
  return dsptest::Db(dsptest::ToneAmplitude(out.data() + 2048, 8192, f, kSr) / 0.5);
}

}  // namespace

SND_TEST(VoiceFilter, FrequencyResponse) {
  const snd::FilterTarget lp{1000.0f, 0.0f};
  SND_CHECK_NEAR(VoiceFilterGain(lp, 100.0), 0.0, 0.05);
  SND_CHECK_NEAR(VoiceFilterGain(lp, 1000.0), -3.01, 0.1);
  SND_CHECK_NEAR(VoiceFilterGain(lp, 4000.0), -24.1, 1.0);  // 12 dB/oct
  const snd::FilterTarget hp{1.0e9f, 1000.0f};
  SND_CHECK_NEAR(VoiceFilterGain(hp, 10000.0), 0.0, 0.05);
  SND_CHECK_NEAR(VoiceFilterGain(hp, 1000.0), -3.01, 0.1);
  SND_CHECK_NEAR(VoiceFilterGain(hp, 250.0), -24.1, 1.0);
  const snd::FilterTarget band{4000.0f, 250.0f};
  SND_CHECK(VoiceFilterGain(band, 1000.0) > -0.6);
  SND_CHECK(VoiceFilterGain(band, 100.0) < -12.0);
  // Open: exact pass-through.
  snd::FilterState st;
  VoiceFilterInit(st, kVoiceFilterOpen);
  SND_CHECK(VoiceFilterIsOpen(st, 48000));
  Audio a(2, 100);
  FillSine(a, 440.0, 0.7f);
  const float keep = a.ch(1)[37];
  VoiceFilterProcess(st, a.p(), 2, 100, kVoiceFilterOpen, 48000);
  SND_CHECK_EQ(a.ch(1)[37], keep);
}

SND_TEST(VoiceFilter, SwitchingOnDoesNotClick) {
  // DC input; the lowpass switching on from "open" must not dip the output.
  snd::FilterState st;
  VoiceFilterInit(st, kVoiceFilterOpen);
  Audio a(1, 256);
  for (int b = 0; b < 4; ++b) {
    for (std::uint32_t i = 0; i < 256; ++i) a.ch(0)[i] = 0.5f;
    const snd::FilterTarget t = b == 0 ? kVoiceFilterOpen : snd::FilterTarget{800.0f, 0.0f};
    VoiceFilterProcess(st, a.p(), 1, 256, t, 48000);
    for (std::uint32_t i = 0; i < 256; ++i) SND_CHECK_NEAR(a.ch(0)[i], 0.5, 1e-3);
  }
  // Highpass switching on at audio rate: no DC step.
  snd::FilterState hs;
  VoiceFilterInit(hs, kVoiceFilterOpen);
  Audio s(1, 256);
  FillSine(s, 1000.0, 0.5f);
  VoiceFilterProcess(hs, s.p(), 1, 256, snd::FilterTarget{1.0e9f, 50.0f}, 48000);
  SND_CHECK(Peak(s.ch(0), 256) < 0.52f);
}

SND_TEST(VoiceFilter, SweepIsSmoothAndBlockInvariant) {
  // Constant cutoff: one block of 256 == blocks 100 + 156.
  snd::FilterState a, b;
  const snd::FilterTarget t{2000.0f, 100.0f};
  VoiceFilterInit(a, t);
  VoiceFilterInit(b, t);
  Audio x(2, 256), y(2, 256);
  dsptest::Noise nz(7);
  for (std::uint32_t c = 0; c < 2; ++c)
    for (std::uint32_t i = 0; i < 256; ++i) x.ch(c)[i] = y.ch(c)[i] = nz.Next(0.8f);
  VoiceFilterProcess(a, x.p(), 2, 256, t, 48000);
  float* p1[2] = {y.ch(0), y.ch(1)};
  float* p2[2] = {y.ch(0) + 100, y.ch(1) + 100};
  VoiceFilterProcess(b, p1, 2, 100, t, 48000);
  VoiceFilterProcess(b, p2, 2, 156, t, 48000);
  SND_CHECK(dsptest::MaxAbsDiff(x.ch(1), y.ch(1), 256) == 0.0f);
  // A fast sweep stays finite and bounded.
  for (int k = 0; k < 50; ++k) {
    for (std::uint32_t i = 0; i < 256; ++i) x.ch(0)[i] = nz.Next(1.0f);
    const float f = (k & 1) ? 15000.0f : 40.0f;
    VoiceFilterProcess(a, x.p(), 1, 256, snd::FilterTarget{f, 0.0f}, 48000);
    SND_CHECK(!HasNonFinite(x.ch(0), 256));
    SND_CHECK(Peak(x.ch(0), 256) < 4.0f);
  }
}

SND_TEST(Biquad, DesignMatchesCookbookResponses) {
  const double sr = 48000.0;
  BiquadCoefs c = DesignBiquad(BiquadType::Lowpass, 1000.0f, 0.70710678f, 0.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 1000.0, sr)), -3.01, 0.02);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 10.0, sr)), 0.0, 0.01);
  c = DesignBiquad(BiquadType::Highpass, 1000.0f, 0.70710678f, 0.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 1000.0, sr)), -3.01, 0.02);
  c = DesignBiquad(BiquadType::Peaking, 2000.0f, 1.0f, 6.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 2000.0, sr)), 6.0, 0.01);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 20.0, sr)), 0.0, 0.05);
  c = DesignBiquad(BiquadType::LowShelf, 200.0f, 0.0f, -12.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 10.0, sr)), -12.0, 0.1);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 200.0, sr)), -6.0, 0.1);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 15000.0, sr)), 0.0, 0.05);
  c = DesignBiquad(BiquadType::HighShelf, 5000.0f, 0.70710678f, 9.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 20000.0, sr)), 9.0, 0.3);
  c = DesignBiquad(BiquadType::Notch, 1000.0f, 2.0f, 0.0f, 48000.0f);
  SND_CHECK(dsptest::Db(BiquadMagnitude(c, 1000.0, sr)) < -60.0);
  c = DesignBiquad(BiquadType::Bandpass, 1000.0f, 2.0f, 0.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 1000.0, sr)), 0.0, 0.01);
  c = DesignBiquad(BiquadType::Allpass, 1000.0f, 2.0f, 0.0f, 48000.0f);
  SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, 3000.0, sr)), 0.0, 0.01);
  SND_CHECK_NEAR(OctaveBandwidthToQ(1.0f), 1.4142, 1e-3);
}

SND_TEST(Biquad, ProcessedSineMatchesDesignedMagnitude) {
  const BiquadCoefs c = DesignBiquad(BiquadType::Peaking, 3000.0f, 2.0f, -9.0f, 48000.0f);
  BiquadState st;
  BiquadReset(st);
  Audio a(1, 256);
  std::vector<float> out;
  for (int b = 0; b < 40; ++b) {
    FillSine(a, 3000.0, 0.5f, static_cast<std::uint64_t>(b) * 256u);
    {
      SND_RT_SCOPE();
      BiquadProcess(st, c, c, a.cp(), a.p(), 1, 256);
    }
    out.insert(out.end(), a.ch(0), a.ch(0) + 256);
  }
  SND_CHECK_NEAR(dsptest::Db(dsptest::ToneAmplitude(out.data() + 2048, 8192, 3000.0, kSr) / 0.5), -9.0, 0.05);
  // Coefficient interpolation between two stable designs stays stable.
  const BiquadCoefs lo = DesignBiquad(BiquadType::Lowpass, 30.0f, 10.0f, 0.0f, 48000.0f);
  const BiquadCoefs hi = DesignBiquad(BiquadType::Lowpass, 20000.0f, 0.5f, 0.0f, 48000.0f);
  dsptest::Noise nz(3);
  for (int b = 0; b < 100; ++b) {
    for (std::uint32_t i = 0; i < 256; ++i) a.ch(0)[i] = nz.Next();
    BiquadProcess(st, (b & 1) ? lo : hi, (b & 1) ? hi : lo, a.cp(), a.p(), 1, 256);
    BiquadFlush(st, 1);
    SND_CHECK(!HasNonFinite(a.ch(0), 256));
  }
}

SND_TEST(Meter, PeakAndRms) {
  PeakMeterState pk;
  PeakMeterInit(pk, 2, 20.0f, 48000);
  RmsMeterState rms;
  RmsMeterInit(rms, 2, 0.3f, 48000);
  Audio a(2, 256);
  for (int b = 0; b < 400; ++b) {
    FillSine(a, 1000.0, 0.5f, static_cast<std::uint64_t>(b) * 256u);
    SND_RT_SCOPE();
    PeakMeterProcess(pk, a.cp(), 256);
    RmsMeterProcess(rms, a.cp(), 256);
  }
  SND_CHECK_NEAR(pk.peak[0], 0.5, 1e-3);
  SND_CHECK_NEAR(RmsMeterValue(rms, 1), 0.5 / std::sqrt(2.0), 2e-3);
  a.Zero();
  for (int b = 0; b < 188; ++b) PeakMeterProcess(pk, a.cp(), 256);  // ~1 s at 20 dB/s
  SND_CHECK_NEAR(dsptest::Db(pk.peak[0] / 0.5), -20.0, 0.3);
}

SND_TEST(Meter, LoudnessEbuReference) {
  // EBU Tech 3341 case 1: stereo 1 kHz sine at -23 dBFS -> -23.0 LUFS (+-0.1).
  auto lm = std::make_unique<LoudnessMeterState>();
  LoudnessMeterInit(*lm, 2, 48000);
  Audio a(2, 256);
  const float amp = static_cast<float>(std::pow(10.0, -23.0 / 20.0));
  for (int b = 0; b < 48000 * 20 / 256; ++b) {
    FillSine(a, 1000.0, amp, static_cast<std::uint64_t>(b) * 256u);
    SND_RT_SCOPE();
    LoudnessMeterProcess(*lm, a.cp(), 256);
  }
  SND_CHECK_NEAR(LoudnessIntegrated(*lm), -23.0, 0.1);
  SND_CHECK_NEAR(LoudnessShortTerm(*lm), -23.0, 0.1);
  SND_CHECK_NEAR(LoudnessMomentary(*lm), -23.0, 0.1);
  // Case 2-like: 10 s at -36 dBFS then 10 s of -23: the relative gate drops the quiet part
  // only if it is more than 10 LU below; here it is 13 LU below -> integrated stays ~-23.
  LoudnessMeterReset(*lm);
  for (int b = 0; b < 48000 * 10 / 256; ++b) {
    FillSine(a, 1000.0, amp * 0.2238721f, static_cast<std::uint64_t>(b) * 256u);
    LoudnessMeterProcess(*lm, a.cp(), 256);
  }
  for (int b = 0; b < 48000 * 10 / 256; ++b) {
    FillSine(a, 1000.0, amp, static_cast<std::uint64_t>(b) * 256u);
    LoudnessMeterProcess(*lm, a.cp(), 256);
  }
  SND_CHECK_NEAR(LoudnessIntegrated(*lm), -23.0, 0.2);
  // Silence: no blocks pass the absolute gate.
  LoudnessMeterReset(*lm);
  a.Zero();
  for (int b = 0; b < 200; ++b) LoudnessMeterProcess(*lm, a.cp(), 256);
  SND_CHECK_EQ(LoudnessIntegrated(*lm), kLoudnessSilence);
  // 5.1: a surround channel weighs +1.5 dB, LFE is ignored.
  LoudnessMeterInit(*lm, 6, 48000);
  Audio s(6, 256);
  for (int b = 0; b < 48000 * 5 / 256; ++b) {
    s.Zero();
    for (std::uint32_t i = 0; i < 256; ++i) {
      const float v = amp * static_cast<float>(std::sin(2 * dsptest::kPi * 1000.0 * (b * 256.0 + i) / kSr));
      s.ch(4)[i] = v;
      s.ch(3)[i] = v;
    }
    LoudnessMeterProcess(*lm, s.cp(), 256);
  }
  SND_CHECK_NEAR(LoudnessIntegrated(*lm), -23.0 - 3.0103 + 1.4922, 0.1);
}

SND_TEST(Meter, SpectrumFindsTheTone) {
  constexpr std::uint32_t kN = 1024;
  dsptest::AlignedMem mem(SpectrumStateBytes(kN), dsptest::kDirty);
  SND_REQUIRE(SpectrumInit(mem.get(), kN));
  SND_CHECK_EQ(SpectrumStateBytes(1000), 0u);
  Audio a(2, 256);
  const double f = 48000.0 * 40.0 / kN;  // bin 40
  std::vector<float> mag(kN / 2 + 1);
  for (int b = 0; b < 8; ++b) {
    FillSine(a, f, 0.5f, static_cast<std::uint64_t>(b) * 256u);
    SND_RT_SCOPE();
    SpectrumPush(mem.get(), a.cp(), 2, 256);
  }
  {
    SND_RT_SCOPE();
    SpectrumCompute(mem.get(), mag.data());
  }
  std::uint32_t best = 0;
  for (std::uint32_t k = 1; k < mag.size(); ++k)
    if (mag[k] > mag[best]) best = k;
  SND_CHECK_EQ(best, 40u);
  SND_CHECK_NEAR(mag[40], 0.5, 0.01);
  SND_CHECK(mag[60] < 1e-4f);
  SND_CHECK_EQ(SpectrumSize(mem.get()), kN);
}
