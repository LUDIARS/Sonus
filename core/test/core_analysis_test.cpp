// core, stage 3 group 4: analysis (4.6, 7 "解析") - per-bus peak / RMS, loudness of the
// output and of kBusMeter buses (D-147), the spectrum (D-148), with known signals.
#include <cmath>
#include <vector>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;
// Buses: Meter (row 0, kBusMeter), Plain (row 1), Quiet (row 2, kBusMeter) -> Master (3).
// Events: S plays a 1 kHz sine (amplitude 0.5) into Meter; P the same into Plain; Q into
// Quiet at -20 dB; St a stereo sine (L 0.4 at 1 kHz, R 0.2 at 3 kHz) into Meter.
TestBank AnalysisBank() {
  TestBank b;
  b.buses.push_back(TBus{HashId("Meter"), 3u, 2, bf::kBusMeter, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Plain"), 3u, 2, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Quiet"), 3u, 2, bf::kBusMeter, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.assets.push_back(TAsset{HashId("sine"), 1, 48000, Sine(480000, 1000.0, 48000.0, 0.5f), 0, 0});
  std::vector<float> st(2 * 480000);
  for (std::size_t i = 0; i < 480000; ++i) {
    st[2 * i] = static_cast<float>(0.4 * std::sin(6.283185307179586 * 1000.0 * double(i) / 48000.0));
    st[2 * i + 1] = static_cast<float>(0.2 * std::sin(6.283185307179586 * 3000.0 * double(i) / 48000.0));
  }
  b.assets.push_back(TAsset{HashId("st"), 2, 48000, st, 0, 0});
  b.events.push_back(SimpleEvent("S", 0, 0));
  b.events.push_back(SimpleEvent("P", 0, 1));
  b.events.push_back(SimpleEvent("Q", 0, 2, -20.0f));
  b.events.push_back(SimpleEvent("St", 1, 0));
  return b;
}
// BS.1770 of a stereo pair of 1 kHz sines with peak amplitudes aL, aR: the K-weighting
// gain at 1 kHz (+0.691 dB) cancels the -0.691 of the formula (a full-scale 1 kHz sine on
// one channel reads -3.01 LUFS): 10 log10(aL^2 / 2 + aR^2 / 2).
double SineLufs(double aL, double aR) { return 10.0 * std::log10(aL * aL / 2.0 + aR * aR / 2.0); }
}  // namespace

// Peak and RMS per bus and per channel: a sine of amplitude a reads a and a / sqrt(2).
SND_TEST(CoreAnalysis, PeakAndRmsPerBusAndChannel) {
  Harness h;
  h.Load("b", AnalysisBank().Build(HashId("B")));
  h.sys->Play(HashId("St"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(700);  // >> the RMS window (0.3 s, exponential)
  h.sys->Update();
  snd_meter m{};
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Meter"), &m), SND_OK);
  SND_CHECK_EQ(m.channels, 2u);
  SND_CHECK_NEAR(m.peak[0], 0.4f, 0.004f);
  SND_CHECK_NEAR(m.peak[1], 0.2f, 0.002f);
  SND_CHECK_NEAR(m.rms[0], 0.4f / std::sqrt(2.0f), 0.004f);
  SND_CHECK_NEAR(m.rms[1], 0.2f / std::sqrt(2.0f), 0.002f);
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Plain"), &m), SND_OK);
  SND_CHECK_NEAR(m.peak[0], 0.5f * kCenter, 0.004f);
  SND_CHECK_NEAR(m.rms[1], 0.5f * kCenter / std::sqrt(2.0f), 0.004f);
}

// D-147: loudness of the output and of each kBusMeter bus on its own; a bus without the
// flag reports NaN (not measured).
SND_TEST(CoreAnalysis, LoudnessPerMeteredBus) {
  Harness h;
  h.Load("b", AnalysisBank().Build(HashId("B")));
  h.sys->Play(HashId("S"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("Q"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(4 * 188);  // 4 s: the 3 s short-term window is full
  h.sys->Update();
  const double a = 0.5 * kCenter;
  snd_meter m{};
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Meter"), &m), SND_OK);
  snd::test::Note("Meter: short %.3f integrated %.3f (expected %.3f)", m.loudness_short, m.loudness_integrated, SineLufs(a, a));
  SND_CHECK_NEAR(m.loudness_short, SineLufs(a, a), 0.2);
  SND_CHECK_NEAR(m.loudness_integrated, SineLufs(a, a), 0.2);
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Quiet"), &m), SND_OK);
  SND_CHECK_NEAR(m.loudness_short, SineLufs(a * 0.1, a * 0.1), 0.2);
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Plain"), &m), SND_OK);
  SND_CHECK(std::isnan(m.loudness_short));
  SND_CHECK(std::isnan(m.loudness_integrated));
  // The output: the three sines in phase (amplitudes add).
  const double sum = a * (2.0 + 0.1);
  SND_REQUIRE_EQ(h.sys->GetMeter(0u, &m), SND_OK);
  SND_CHECK_NEAR(m.loudness_short, SineLufs(sum, sum), 0.2);
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Master"), &m), SND_OK);
  SND_CHECK_NEAR(m.loudness_integrated, SineLufs(sum, sum), 0.2);
}

// D-148: the spectrum of a known tone: the peak at its bin, reading the amplitude; per
// channel; only for the output and kBusMeter buses; the errors.
SND_TEST(CoreAnalysis, SpectrumOfAKnownTone) {
  Harness h;
  TestBank b = AnalysisBank();
  // A tone centred on bin 32 of the 1024-point FFT: 32 * 48000 / 1024 = 1500 Hz.
  b.assets.push_back(TAsset{HashId("bin"), 1, 48000, Sine(480000, 1500.0, 48000.0, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("Bin", 2, 0));
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("Bin"), snd_emitter_handle{0}, nullptr);
  std::vector<float> mag(600, -1.0f);
  // Not read so far: no data yet; asking starts it (ready after the next Update).
  SND_CHECK_EQ(h.sys->GetSpectrum(0u, 0u, mag.data(), 513u), SND_ERR_NOT_READY);
  h.Steps(10);
  SND_REQUIRE_EQ(h.sys->GetSpectrum(0u, 0u, mag.data(), 600u), SND_OK);
  std::uint32_t peak = 0;
  for (std::uint32_t k = 0; k < 513; ++k) peak = mag[k] > mag[peak] ? k : peak;
  SND_CHECK_EQ(peak, 32u);
  SND_CHECK_NEAR(mag[32], 0.5f * kCenter, 0.005f);
  SND_CHECK(mag[40] < 1e-3f && mag[10] < 1e-3f);
  for (std::uint32_t k = 513; k < 600; ++k) SND_CHECK_EQ(mag[k], 0.0f);
  // A metered bus, channel 1; the master by its id is the output.
  SND_REQUIRE_EQ(h.sys->GetSpectrum(HashId("Meter"), 1u, mag.data(), 513u), SND_OK);
  SND_CHECK_NEAR(mag[32], 0.5f * kCenter, 0.005f);
  SND_REQUIRE_EQ(h.sys->GetSpectrum(HashId("Master"), 0u, mag.data(), 513u), SND_OK);
  SND_CHECK_NEAR(mag[32], 0.5f * kCenter, 0.005f);
  // Errors.
  SND_CHECK_EQ(h.sys->GetSpectrum(HashId("Plain"), 0u, mag.data(), 513u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(h.sys->GetSpectrum(HashId("Meter"), 2u, mag.data(), 513u), SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(h.sys->GetSpectrum(HashId("Nope"), 0u, mag.data(), 513u), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(h.sys->GetSpectrum(0u, 0u, nullptr, 513u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->GetSpectrum(0u, 0u, mag.data(), 0u), SND_ERR_INVALID_ARGUMENT);
  // Two tones on two channels of a metered bus.
  h.sys->Play(HashId("St"), snd_emitter_handle{0}, nullptr);
  h.Steps(10);
  SND_REQUIRE_EQ(h.sys->GetSpectrum(HashId("Meter"), 0u, mag.data(), 513u), SND_OK);
  const float l1k = mag[21] + mag[22];  // 1 kHz lies between bins 21 and 22
  SND_REQUIRE_EQ(h.sys->GetSpectrum(HashId("Meter"), 1u, mag.data(), 513u), SND_OK);
  SND_CHECK(l1k > 0.25f);
  SND_CHECK(mag[64] > 0.15f);  // 3 kHz = bin 64 on the right channel (0.2)
}

// Without readers the spectrum is not kept: after the interest ran out, a read reports
// NOT_READY (not an old spectrum) and the data come back after the next Update.
SND_TEST(CoreAnalysis, SpectrumInterestExpires) {
  Harness h;
  h.Load("b", AnalysisBank().Build(HashId("B")));
  h.sys->Play(HashId("S"), snd_emitter_handle{0}, nullptr);
  std::vector<float> mag(513);
  h.sys->GetSpectrum(0u, 0u, mag.data(), 513u);
  h.Steps(4);
  SND_CHECK_EQ(h.sys->GetSpectrum(0u, 0u, mag.data(), 513u), SND_OK);
  h.Steps(300);  // > 256 blocks without a read
  SND_CHECK_EQ(h.sys->GetSpectrum(0u, 0u, mag.data(), 513u), SND_ERR_NOT_READY);
  h.Steps(2);
  SND_CHECK_EQ(h.sys->GetSpectrum(0u, 0u, mag.data(), 513u), SND_OK);
}

// More kBusMeter buses than meters: the first ones measured have loudness, the rest NaN.
SND_TEST(CoreAnalysis, MeteredBusesBeyondTheLimitHaveNoLoudness) {
  snd::core::CoreLimits lim;
  lim.maxMeteredBuses = 1;
  Harness h(SmallConfig(), lim);
  h.Load("b", AnalysisBank().Build(HashId("B")));
  h.sys->Play(HashId("S"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("Q"), snd_emitter_handle{0}, nullptr);
  h.Steps(100);
  h.sys->Update();
  snd_meter m1{}, m2{};
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Meter"), &m1), SND_OK);
  SND_REQUIRE_EQ(h.sys->GetMeter(HashId("Quiet"), &m2), SND_OK);
  SND_CHECK(std::isnan(m1.loudness_short) != std::isnan(m2.loudness_short));
}
