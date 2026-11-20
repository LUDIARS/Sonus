// Oscillator kind (stage 7-D, D-190 .. D-192) driven directly (synth_driver.hpp):
// frequency and amplitude by DFT against the Fourier series of each waveform, the
// aliasing folded below Nyquist compared with the naive waveform, the pitch ramp
// (phase = the integral of the frequency, no jump at block edges), duty and table
// position modulation, virtual advance and seek, block-size independence.
#include <cmath>
#include <cstdio>
#include <vector>

#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;
namespace bf = snd::bank_format;

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kRate = 48000.0;

src::OscillatorParams Osc(bf::Waveform w, float hz, float duty = 0.5f) {
  src::OscillatorParams p{};
  p.waveform = static_cast<std::uint32_t>(w);
  p.frequencyHz = hz;
  p.duty = duty;
  p.tablePosition = 0.0f;
  p.wavetable = nullptr;
  return p;
}

std::vector<float> Render(const src::OscillatorParams& p, std::size_t frames, const Driver::Mod& m = {},
                          std::uint32_t block = 256) {
  Driver d(snd::SourceKind::Oscillator, Driver::Opt{});
  Driver::Voice v;
  if (!d.Ok() || !d.Start(v, &p, 1)) return {};
  d.Render(v, frames, m, block);
  return v.out[0];
}

// Naive (not band-limited) waveforms with the kind's start phases.
double Naive(bf::Waveform w, double phase, double duty) {
  switch (w) {
    case bf::Waveform::Saw: return 2.0 * phase - 1.0;
    case bf::Waveform::Square: return phase < 0.5 ? 1.0 : -1.0;
    case bf::Waveform::Triangle: return phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
    case bf::Waveform::Pulse: return (phase < duty ? 1.0 : -1.0) - (2.0 * duty - 1.0);
    default: return std::sin(2.0 * kPi * phase);
  }
}
std::vector<float> NaiveSignal(bf::Waveform w, double hz, double startPhase, std::size_t n, double duty = 0.5) {
  std::vector<float> x(n);
  double ph = startPhase;
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(Naive(w, ph, duty));
    ph += hz / kRate;
    ph -= std::floor(ph);
  }
  return x;
}

}  // namespace

// Fundamental and first harmonics against the Fourier series (peak 1 waveforms).
SND_TEST(SourceOscillator, FrequencyAndAmplitudeMatchTheFourierSeries) {
  const double f = 1000.0;
  const std::size_t n = 9600;  // 200 periods
  struct Case {
    bf::Waveform w;
    float duty;
    double h1, h2, h3;  // amplitudes of f, 2f, 3f
  };
  const double d = 0.25;
  const Case cases[] = {
      {bf::Waveform::Sine, 0.5f, 1.0, 0.0, 0.0},
      {bf::Waveform::Saw, 0.5f, 2.0 / kPi, 1.0 / kPi, 2.0 / (3.0 * kPi)},
      {bf::Waveform::Square, 0.5f, 4.0 / kPi, 0.0, 4.0 / (3.0 * kPi)},
      {bf::Waveform::Triangle, 0.5f, 8.0 / (kPi * kPi), 0.0, 8.0 / (9.0 * kPi * kPi)},
      {bf::Waveform::Pulse, static_cast<float>(d), 4.0 / kPi * std::sin(kPi * d), 2.0 / kPi * std::sin(2 * kPi * d),
       4.0 / (3.0 * kPi) * std::sin(3 * kPi * d)},
  };
  for (const Case& c : cases) {
    const std::vector<float> x = Render(Osc(c.w, static_cast<float>(f), c.duty), n + 512);
    SND_REQUIRE(x.size() >= n);
    const double a1 = Tone(x, 0, n, f), a2 = Tone(x, 0, n, 2 * f), a3 = Tone(x, 0, n, 3 * f);
    SND_CHECK_MSG(std::fabs(a1 - c.h1) < 0.01 * c.h1 + 1e-3, "wave %u: f %f want %f", unsigned(c.w), a1, c.h1);
    SND_CHECK_MSG(std::fabs(a2 - c.h2) < 0.015 * (c.h2 + 0.05), "wave %u: 2f %f want %f", unsigned(c.w), a2, c.h2);
    SND_CHECK_MSG(std::fabs(a3 - c.h3) < 0.015 * (c.h3 + 0.05), "wave %u: 3f %f want %f", unsigned(c.w), a3, c.h3);
    // Tones in between are absent; the pulse has no DC.
    SND_CHECK_MSG(Tone(x, 0, n, 1500.0) < 2e-3, "wave %u: 1.5 f", unsigned(c.w));
    double mean = 0;
    for (std::size_t i = 0; i < n; ++i) mean += x[i];
    SND_CHECK_MSG(std::fabs(mean / double(n)) < 2e-3, "wave %u: DC %f", unsigned(c.w), mean / double(n));
  }
}

// PolyBLEP / PolyBLAMP: what folds back below Nyquist is well below the naive waveform.
SND_TEST(SourceOscillator, BandLimitingReducesAliasing) {
  const std::size_t n = 16384;
  struct Case {
    bf::Waveform w;
    double start;
    float duty;
    double minGainDb;  // naive alias share - band-limited alias share
    double maxAliasDb; // band-limited alias share (relative to the total power)
  };
  const Case cases[] = {
      // 2-point PolyBLEP: 13-22 dB less than naive, -23..-33 dB of the power at 1-7 kHz.
      {bf::Waveform::Saw, 0.5, 0.5f, 12.0, -20.0},
      {bf::Waveform::Square, 0.0, 0.5f, 12.0, -20.0},
      {bf::Waveform::Pulse, 0.0, 0.3f, 12.0, -20.0},
      {bf::Waveform::Triangle, 0.25, 0.5f, 12.0, -40.0},
  };
  for (double f : {1234.5, 3011.0, 7001.3}) {
    for (const Case& c : cases) {
      const std::vector<float> bl = Render(Osc(c.w, static_cast<float>(f), c.duty), n);
      SND_REQUIRE(bl.size() == n);
      const std::vector<float> nv = NaiveSignal(c.w, f, c.start, n, c.duty);
      const double aBl = AliasDb(Power(bl, 0, n), f, kRate, n);
      const double aNv = AliasDb(Power(nv, 0, n), f, kRate, n);
      std::printf("alias wave %u f %.1f: band-limited %.1f dB, naive %.1f dB\n", unsigned(c.w), f, aBl, aNv);
      SND_CHECK_MSG(aNv - aBl > c.minGainDb, "wave %u f %f: %f vs naive %f", unsigned(c.w), f, aBl, aNv);
      SND_CHECK_MSG(aBl < c.maxAliasDb, "wave %u f %f: alias %f dB", unsigned(c.w), f, aBl);
    }
  }
}

// The pitch ramp is applied per sample: the phase is the running sum of
// f (p0 + (p1 - p0) i / B) / rate, continuous over block edges.
SND_TEST(SourceOscillator, PitchRampIsIntegratedPerSample) {
  const double f = 440.0;
  Driver d(snd::SourceKind::Oscillator, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  const src::OscillatorParams p = Osc(bf::Waveform::Sine, static_cast<float>(f));
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 2));
  const float ramps[][2] = {{1.0f, 1.0f}, {1.0f, 2.0f}, {2.0f, 2.0f}, {2.0f, 0.5f}, {0.5f, 0.5f}, {0.5f, 1.5f}};
  std::vector<double> want;
  double phase = 0.0;
  for (const auto& r : ramps) {
    Driver::Mod m;
    m.pitch = snd::Ramp{r[0], r[1]};
    d.Block({&v}, m);
    for (int i = 0; i < 256; ++i) {
      want.push_back(std::sin(2.0 * kPi * phase));
      const double p01 = r[0] + (double(r[1]) - r[0]) * double(i) / 256.0;
      phase += f * p01 / kRate;
      phase -= std::floor(phase);
    }
  }
  SND_REQUIRE_EQ(v.out[0].size(), want.size());
  double err = 0;
  for (std::size_t i = 0; i < want.size(); ++i) err = std::fmax(err, std::fabs(v.out[0][i] - want[i]));
  SND_CHECK_MSG(err < 1e-5, "max error %g", err);
  // Both channels carry the same signal; the playhead counts output frames.
  SND_CHECK(v.out[1] == v.out[0]);
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{6 * 256});
  // A constant ratio of 2 doubles the frequency.
  Driver::Mod m2;
  m2.pitch = snd::Ramp{2.0f, 2.0f};
  const std::vector<float> x = Render(p, 9600, m2);
  SND_CHECK_NEAR(Tone(x, 0, 9600, 880.0), 1.0, 0.01);
}

// Sample-accurate start inside a block: frames before frameOffset stay silent and the
// waveform starts there at its start phase.
SND_TEST(SourceOscillator, FrameOffsetStartsInsideTheBlock) {
  Driver d(snd::SourceKind::Oscillator, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  const src::OscillatorParams p = Osc(bf::Waveform::Sine, 1000.0f);
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Block({&v}, Driver::Mod{}, 0, 100);
  SND_REQUIRE_EQ(v.out[0].size(), std::size_t{156});
  for (std::size_t i = 0; i < 156; ++i) {
    SND_CHECK_NEAR(v.out[0][i], std::sin(2.0 * kPi * 1000.0 * double(i) / kRate), 1e-5);
  }
}

// Same output whatever the block size (constant pitch): the phase is continuous.
SND_TEST(SourceOscillator, BlockSizeDoesNotChangeTheOutput) {
  for (bf::Waveform w : {bf::Waveform::Sine, bf::Waveform::Saw, bf::Waveform::Square, bf::Waveform::Triangle,
                         bf::Waveform::Pulse}) {
    const src::OscillatorParams p = Osc(w, 777.7f, 0.3f);
    const std::vector<float> a = Render(p, 5000, Driver::Mod{}, 256);
    const std::vector<float> b = Render(p, 5000, Driver::Mod{}, 37);
    SND_REQUIRE_EQ(a.size(), b.size());
    double err = 0;
    for (std::size_t i = 0; i < a.size(); ++i) err = std::fmax(err, std::fabs(a[i] - b[i]));
    SND_CHECK_MSG(err == 0.0, "wave %u: %g", unsigned(w), err);
  }
}

// AdvanceVirtual moves the phase by the time the voice was virtual (at its pitch);
// Seek puts it where an unmodulated voice would be.
SND_TEST(SourceOscillator, VirtualAdvanceAndSeekKeepThePhase) {
  const src::OscillatorParams p = Osc(bf::Waveform::Saw, 523.25f);
  const std::vector<float> ref = Render(p, 20000);
  SND_REQUIRE(ref.size() == 20000u);
  Driver d(snd::SourceKind::Oscillator, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1));
  d.Render(v, 2560);
  d.Advance(v, 5120);
  SND_CHECK_EQ(v.ph.frame, std::uint64_t{7680});
  const std::size_t before = v.out[0].size();
  d.Render(v, 2560);
  double err = 0;
  for (std::size_t i = 0; i < 2560; ++i) err = std::fmax(err, std::fabs(v.out[0][before + i] - ref[7680 + i]));
  SND_CHECK_MSG(err < 1e-4, "after advance %g", err);
  // Advance at pitch 2 = twice as many periods.
  d.Advance(v, 1000, 2.0f);
  d.Control(v, src::ControlOp::Seek, 15000);
  const std::size_t at = v.out[0].size();
  d.Render(v, 2000);
  err = 0;
  for (std::size_t i = 0; i < 2000; ++i) err = std::fmax(err, std::fabs(v.out[0][at + i] - ref[15000 + i]));
  SND_CHECK_MSG(err < 1e-4, "after seek %g", err);
  // Pause / Resume / Virtualize / Devirtualize / SetLoop change nothing.
  for (src::ControlOp op : {src::ControlOp::Pause, src::ControlOp::Resume, src::ControlOp::Virtualize,
                            src::ControlOp::Devirtualize, src::ControlOp::SetLoop}) {
    d.Control(v, op, 0);
  }
  const std::size_t at2 = v.out[0].size();
  d.Render(v, 1000);
  err = 0;
  for (std::size_t i = 0; i < 1000; ++i) err = std::fmax(err, std::fabs(v.out[0][at2 + i] - ref[17000 + i]));
  SND_CHECK_MSG(err < 1e-4, "after other ops %g", err);
  SND_CHECK_EQ(v.last.ended, 0u);
}

// Duty modulation (RenderItem::dutyOffset): the fundamental follows 4/pi sin(pi d).
SND_TEST(SourceOscillator, DutyOffsetMovesThePulseWidth) {
  for (float off : {-0.3f, 0.0f, 0.2f}) {
    Driver::Mod m;
    m.duty = snd::Ramp{off, off};
    const std::vector<float> x = Render(Osc(bf::Waveform::Pulse, 1000.0f, 0.5f), 9600, m);
    const double d = 0.5 + off;
    SND_CHECK_NEAR(Tone(x, 0, 9600, 1000.0), 4.0 / kPi * std::sin(kPi * d), 0.01);
    SND_CHECK_NEAR(Tone(x, 0, 9600, 2000.0), std::fabs(2.0 / kPi * std::sin(2 * kPi * d)), 0.01);
  }
  // The square ignores the duty.
  Driver::Mod m;
  m.duty = snd::Ramp{0.3f, 0.3f};
  const std::vector<float> sq = Render(Osc(bf::Waveform::Square, 1000.0f, 0.1f), 9600, m);
  SND_CHECK_NEAR(Tone(sq, 0, 9600, 1000.0), 4.0 / kPi, 0.01);
  SND_CHECK(Tone(sq, 0, 9600, 2000.0) < 0.005);
}

// Unknown waveform, no params: refused (core ends the playback).
SND_TEST(SourceOscillator, BadStartsAreRefused) {
  Driver d(snd::SourceKind::Oscillator, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  Driver::Voice v;
  src::OscillatorParams p = Osc(bf::Waveform::Sine, 100.0f);
  p.waveform = 9;
  SND_CHECK(!d.Start(v, &p, 1));
  SND_CHECK(!d.Start(v, nullptr, 1));
  // A non-PCM / non-resident wavetable.
  src::SourceAsset a{};
  a.form = src::LoadForm::Stream;
  p = Osc(bf::Waveform::Sine, 100.0f);
  p.wavetable = &a;
  SND_CHECK(!d.Start(v, &p, 1));
  // Voice limit.
  Driver::Opt o;
  o.maxVoices = 2;
  Driver d2(snd::SourceKind::Oscillator, o);
  SND_REQUIRE(d2.Ok());
  Driver::Voice a1, a2, a3;
  p = Osc(bf::Waveform::Sine, 100.0f);
  SND_CHECK(d2.Start(a1, &p, 1));
  SND_CHECK(d2.Start(a2, &p, 1));
  SND_CHECK(!d2.Start(a3, &p, 1));
  SND_CHECK_EQ(a3.endDetail, std::uint32_t{SND_END_DETAIL_VOICE_LIMIT});
  d2.Release(a1);
  SND_CHECK(d2.Start(a3, &p, 1));
}

// Wavetable (D-192): resident PCMF, two tables of 2048 frames (sin x, sin 3x);
// position 0 / 1 / 0.5 select / mix them.
SND_TEST(SourceOscillator, WavetableReadsAndMixesTables) {
  const std::uint32_t T = 2048;
  std::vector<float> pcm(2 * T);
  for (std::uint32_t j = 0; j < T; ++j) {
    pcm[j] = static_cast<float>(std::sin(2.0 * kPi * j / T));
    pcm[T + j] = static_cast<float>(std::sin(2.0 * kPi * 3.0 * j / T));
  }
  src::SourceAsset a{};
  a.codec.codec = SND_CODEC_PCM_F32;
  a.codec.channels = 1;
  a.codec.sample_rate = 48000;
  a.codec.total_frames = 2 * T;
  a.data = reinterpret_cast<const std::uint8_t*>(pcm.data());
  a.dataBytes = pcm.size() * 4u;
  a.form = src::LoadForm::PcmResident;
  src::OscillatorParams p = Osc(bf::Waveform::Sine, 500.0f);
  p.wavetable = &a;
  for (float pos : {0.0f, 1.0f, 0.5f, 0.25f}) {
    Driver::Mod m;
    m.table = snd::Ramp{pos, pos};
    const std::vector<float> x = Render(p, 9600, m);
    SND_REQUIRE(x.size() == 9600u);
    SND_CHECK_NEAR(Tone(x, 0, 9600, 500.0), 1.0 - pos, 0.01);
    SND_CHECK_NEAR(Tone(x, 0, 9600, 1500.0), pos, 0.01);
  }
  // A loop marks the table length: one table of 1000 frames (the loop), frames / 1000 tables.
  std::vector<float> one(3000);
  for (std::uint32_t j = 0; j < 3000; ++j) one[j] = static_cast<float>(std::sin(2.0 * kPi * (j % 1000) / 1000.0));
  src::SourceAsset b = a;
  b.codec.total_frames = 3000;
  b.data = reinterpret_cast<const std::uint8_t*>(one.data());
  b.dataBytes = one.size() * 4u;
  b.loopStart = 1000;
  b.loopEnd = 2000;
  p.wavetable = &b;
  const std::vector<float> y = Render(p, 9600);
  SND_CHECK_NEAR(Tone(y, 0, 9600, 500.0), 1.0, 0.01);
}

// D-198: an effective frequency of 0 (0, negative, NaN frequency; 0 / NaN / Inf pitch) is
// silence and the phase waits; non-finite offsets count as 0; never NaN.
SND_TEST(SourceOscillator, ZeroOrNonFiniteFrequencyIsSilenceAndThePhaseWaits) {
  for (bf::Waveform w : {bf::Waveform::Sine, bf::Waveform::Saw, bf::Waveform::Square, bf::Waveform::Triangle,
                         bf::Waveform::Pulse}) {
    for (float f : {0.0f, -100.0f, std::nanf(""), INFINITY}) {
      const std::vector<float> x = Render(Osc(w, f, 0.2f), 1000);
      SND_REQUIRE(x.size() == 1000u);
      for (float v : x) SND_CHECK_MSG(v == 0.0f, "wave %u f %f: %f", unsigned(w), double(f), double(v));
    }
    // Pitch 0 then NaN then Inf for a block each, then 1: the output is the
    // uninterrupted waveform delayed by three blocks.
    const src::OscillatorParams p = Osc(w, 700.0f, 0.3f);
    const std::vector<float> ref = Render(p, 1024);
    Driver d(snd::SourceKind::Oscillator, Driver::Opt{});
    Driver::Voice v;
    SND_REQUIRE(d.Start(v, &p, 1));
    for (float pv : {0.0f, std::nanf(""), INFINITY}) {
      Driver::Mod m;
      m.pitch = snd::Ramp{pv, pv};
      m.duty = snd::Ramp{std::nanf(""), 0.0f};
      m.table = snd::Ramp{INFINITY, 0.0f};
      d.Block({&v}, m);
    }
    Driver::Mod bad;
    bad.duty = snd::Ramp{std::nanf(""), std::nanf("")};
    d.Render(v, 1024, bad);
    for (std::size_t i = 0; i < 768; ++i) SND_CHECK_EQ(v.out[0][i], 0.0f);
    for (std::size_t i = 0; i < 1024; ++i) {
      SND_CHECK_MSG(v.out[0][768 + i] == ref[i], "wave %u at %u", unsigned(w), unsigned(i));
    }
  }
}

// D-198: with a loop, table k starts at loopStart + k x (loopEnd - loopStart).
SND_TEST(SourceOscillator, WavetableTablesStartAtTheLoopStart) {
  // 500 frames of DC 0.9, then two tables of 1000: sin x, sin 2x.
  std::vector<float> pcm(2500, 0.9f);
  for (std::uint32_t j = 0; j < 1000; ++j) {
    pcm[500 + j] = static_cast<float>(std::sin(2.0 * kPi * j / 1000.0));
    pcm[1500 + j] = static_cast<float>(std::sin(2.0 * kPi * 2.0 * j / 1000.0));
  }
  src::SourceAsset a{};
  a.codec.codec = SND_CODEC_PCM_F32;
  a.codec.channels = 1;
  a.codec.sample_rate = 48000;
  a.codec.total_frames = 2500;
  a.data = reinterpret_cast<const std::uint8_t*>(pcm.data());
  a.dataBytes = pcm.size() * 4u;
  a.form = src::LoadForm::PcmResident;
  a.loopStart = 500;
  a.loopEnd = 1500;
  src::OscillatorParams p = Osc(bf::Waveform::Sine, 400.0f);
  p.wavetable = &a;
  const std::vector<float> x0 = Render(p, 9600);
  SND_CHECK_NEAR(Tone(x0, 0, 9600, 400.0), 1.0, 0.01);
  SND_CHECK(Tone(x0, 0, 9600, 0.0) < 0.01);
  Driver::Mod m;
  m.table = snd::Ramp{1.0f, 1.0f};
  const std::vector<float> x1 = Render(p, 9600, m);
  SND_CHECK_NEAR(Tone(x1, 0, 9600, 800.0), 1.0, 0.01);
}
