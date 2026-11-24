// Independent tests: the 14 standard effects through core (a bank puts the effect on a
// bus, voices sound through it), measured as physical quantities (delay in samples, decay
// ratios, RT60, threshold / ratio, ceiling, frequency response, pitch ratio), runtime
// changes (D-130) of one parameter per kind, extreme values on mono / stereo / 5.1 buses,
// and the standard per-voice lowpass / highpass (clip random filter, parameter curve,
// distance curve). Values and measurements chosen independently of the implementer's.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "indep_g2_fixture.hpp"
#include "snd/dsp/effects.hpp"

namespace {

using namespace coretest;  // TestBank & co
using snd::HashId;
namespace dsp = snd::dsp;

constexpr double kRate = 48000.0;
constexpr float kC = 0.70710678f;  // centre pan of a mono voice into stereo

snd_config KConfig(std::uint32_t outChannels = 2) {
  snd_config c = g2::G2Config();  // load LOD off: effects never bypassed
  c.output_channels = outChannels;
  c.max_voice_channels = 2;
  return c;
}

// One bus "Sfx" (channels = busChannels, 0 = the master's) with `chain` effects -> Master.
// Voices come from G2Kind (indep_g2_fixture.hpp): analytic signals at the output rate.
struct Rig {
  Rig(std::vector<TEffect> chain, std::uint32_t busChannels = 0, std::uint32_t outChannels = 2)
      : h(KConfig(outChannels)) {
    b.buses.push_back(TBus{HashId("Sfx"), 1u, busChannels, 0, 0.0f, {}, {}});
    b.buses.push_back(TBus{HashId("Master"), kNone, outChannels, bf::kBusMaster, 0.0f, {}, {}});
    for (std::size_t i = 0; i < chain.size(); ++i) {
      b.effects.push_back(chain[i]);
      b.buses[0].effects.push_back(static_cast<std::uint32_t>(i));
    }
  }
  // A synth event with the G2Kind code `f`.
  void AddSig(const char* name, float f, std::uint32_t channels = 1) {
    b.synths.push_back(TSynth{0, 0, channels, f});
    TEvent e;
    e.id = HashId(name);
    TTrack t;
    TClip c;
    c.kind = 2;
    c.target = static_cast<std::uint32_t>(b.synths.size() - 1);
    t.clips.push_back(c);
    e.tracks.push_back(t);
    b.events.push_back(e);
  }
  void AddImpulse(const char* name = "Imp", float amp = 1.0f) { AddSig(name, amp == 1.0f ? 1000.0f : 1000.0f + amp * 1000.0f); }
  void AddSine(const char* name, float hz) { AddSig(name, 1e6f + hz); }
  bool Load() {
    if (h.sys == nullptr) return false;
    h.Load("k", b.Build(HashId("K")));
    return true;
  }
  snd_playback_handle Play(const char* e) { return h.sys->Play(HashId(e), snd_emitter_handle{0}, nullptr); }
  // Plays `e` and renders `blocks`; returns the first frame of the voice.
  std::size_t PlayAndRender(const char* e, std::uint32_t blocks) {
    const std::size_t f0 = h.Total();
    Play(e);
    h.Steps(blocks);
    return f0;
  }
  std::vector<float> Channel(std::size_t from, std::size_t n, std::uint32_t c = 0) const {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = h.At(from + i, c);
    return v;
  }
  double Amp(std::size_t from, std::size_t n, double hz, std::uint32_t c = 0) const {
    return g2::ToneOf(h.all, h.Ch(), c, from, n, hz, kRate);
  }
  float MaxAbs(std::size_t from, std::size_t to, std::uint32_t c = 0) const {
    float m = 0;
    for (std::size_t f = from; f < to && f < h.Total(); ++f) m = std::fmax(m, std::fabs(h.At(f, c)));
    return m;
  }
  std::size_t ArgMaxAbs(std::size_t from, std::size_t to, std::uint32_t c = 0) const {
    float m = -1;
    std::size_t at = from;
    for (std::size_t f = from; f < to && f < h.Total(); ++f) {
      if (std::fabs(h.At(f, c)) > m) {
        m = std::fabs(h.At(f, c));
        at = f;
      }
    }
    return at;
  }
  g2::RawFx h;
  TestBank b;
};

std::uint32_t Blocks(double seconds) { return static_cast<std::uint32_t>(seconds * kRate / 256.0) + 2u; }
double Db(double x) { return 20.0 * std::log10(x > 1e-30 ? x : 1e-30); }

std::vector<float> ReverbParams(float decay, float dry = -10000.0f, float room = 0.0f) {
  // Dry, Room, RoomHF, RoomLF, Decay, HFRatio, Reflections, ReflDelay, Reverb, RevDelay, Diff, Dens, HFRef, LFRef
  return {dry, room, 0.0f, 0.0f, decay, 1.0f, -10000.0f, 0.0f, 0.0f, 0.02f, 100.0f, 100.0f, 5000.0f, 250.0f};
}

// RT60 from the Schroeder integral of x (slope between -5 dB and -35 dB, extrapolated).
double Rt60(const std::vector<float>& x) {
  std::vector<double> e(x.size() + 1, 0.0);
  for (std::size_t i = x.size(); i-- > 0;) e[i] = e[i + 1] + double(x[i]) * x[i];
  const double total = e[0];
  std::size_t a = 0, z = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double db = 10.0 * std::log10(e[i] / total + 1e-300);
    if (a == 0 && db <= -5.0) a = i;
    if (db <= -35.0) {
      z = i;
      break;
    }
  }
  if (a == 0 || z <= a) return -1.0;
  return 60.0 / (30.0 / (double(z - a) / kRate));
}

}  // namespace

// --- Reverb (I3DL2) -----------------------------------------------------------------
SND_TEST(IndepG2Kinds, ReverbDecayTimeIsTheRt60) {
  for (const float decay : {0.6f, 2.5f}) {
    Rig r({TEffect{dsp::kEffectReverb, ReverbParams(decay)}});
    r.AddImpulse();
    SND_REQUIRE(r.Load());
    const std::size_t f0 = r.PlayAndRender("Imp", Blocks(decay * 1.6));
    const std::vector<float> x = r.Channel(f0, std::size_t(decay * 1.6 * kRate));
    const double rt = Rt60(x);
    std::printf("  DecayTime %.2f s: RT60 %.3f s\n", decay, rt);
    SND_CHECK_NEAR(rt, decay, 0.25 * decay);
  }
}

SND_TEST(IndepG2Kinds, ReverbLevelsDryAndWetOff) {
  // Wet fully off (Room -10000): only the dry impulse.
  {
    Rig r({TEffect{dsp::kEffectReverb, ReverbParams(1.0f, 0.0f, -10000.0f)}});
    r.AddImpulse();
    SND_REQUIRE(r.Load());
    const std::size_t f0 = r.PlayAndRender("Imp", 40);
    SND_CHECK_NEAR(r.h.At(f0, 0), kC, 1e-3);
    SND_CHECK(r.MaxAbs(f0 + 8, f0 + 9000) < 1e-4f);
  }
  // Dry off, wet on: nothing at the impulse, the tail later; predelay respected
  // (reflections off, ReflectionsDelay 0.05 + ReverbDelay 0.05: nothing in the first 90 ms).
  {
    std::vector<float> p = ReverbParams(1.0f);
    p[7] = 0.05f;
    p[9] = 0.05f;
    Rig r({TEffect{dsp::kEffectReverb, p}});
    r.AddImpulse();
    SND_REQUIRE(r.Load());
    const std::size_t f0 = r.PlayAndRender("Imp", 80);
    const float tail = r.MaxAbs(f0 + 4800, f0 + 20000);
    SND_CHECK(tail > 1e-3f);
    SND_CHECK_MSG(r.MaxAbs(f0, f0 + 4320) < tail * 1e-3f, "before the predelay: %g (tail %g)", r.MaxAbs(f0, f0 + 4320),
                  tail);
  }
}

SND_TEST(IndepG2Kinds, ReverbDecayChangedAtRunTime) {
  Rig r({TEffect{dsp::kEffectReverb, ReverbParams(0.5f)}});
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  SND_REQUIRE_EQ(r.h.sys->SetEffectParam(HashId("Sfx"), 0, 4, 2.0f, 0.0f), SND_OK);
  r.h.Steps(2);
  const std::size_t f0 = r.PlayAndRender("Imp", Blocks(3.2));
  const double rt = Rt60(r.Channel(f0, std::size_t(3.2 * kRate)));
  std::printf("  DecayTime 0.5 -> 2.0 at run time: RT60 %.3f s\n", rt);
  SND_CHECK_NEAR(rt, 2.0, 0.5);
}

// --- Echo ---------------------------------------------------------------------------
SND_TEST(IndepG2Kinds, EchoDelayInSamplesAndDecayRatio) {
  Rig r({TEffect{dsp::kEffectEcho, {120.0f, 0.4f, 1.0f, 1.0f, 200.0f}}});
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("Imp", 100);
  SND_CHECK_NEAR(r.h.At(f0, 0), kC, 1e-5);  // dry
  float amp[4];
  for (int k = 1; k <= 3; ++k) {
    const std::size_t at = f0 + 5760u * std::size_t(k);
    SND_CHECK_EQ(r.ArgMaxAbs(at - 200, at + 200), at);
    amp[k] = r.h.At(at, 0);
  }
  std::printf("  echo amplitudes: %.5f %.5f %.5f (of %.5f)\n", amp[1], amp[2], amp[3], kC);
  SND_CHECK_NEAR(amp[2] / amp[1], 0.4, 1e-3);
  SND_CHECK_NEAR(amp[3] / amp[2], 0.4, 1e-3);
  SND_CHECK(r.MaxAbs(f0 + 1, f0 + 5760 - 1) < 1e-6f);
  // DryMix 0 at run time: no dry at the next impulse.
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, 0.0f);
  r.h.Steps(2);
  const std::size_t f1 = r.PlayAndRender("Imp", 30);
  SND_CHECK(std::fabs(r.h.At(f1, 0)) < 1e-6f);
  // Delay 60 ms at run time: the next echo comes after 2880 frames.
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 60.0f, 0.0f);
  r.h.Steps(120);
  const std::size_t f2 = r.PlayAndRender("Imp", 30);
  SND_CHECK_EQ(r.ArgMaxAbs(f2 + 100, f2 + 5000), f2 + 2880);
}

// --- Delay --------------------------------------------------------------------------
SND_TEST(IndepG2Kinds, DelayTimeFeedbackAndLevels) {
  Rig r({TEffect{dsp::kEffectDelay, {75.0f, 0.5f, -96.0f, -6.0f, 20000.0f, 0.0f, 100.0f}}});
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("Imp", 80);
  SND_CHECK_NEAR(r.h.At(f0, 0), kC * std::pow(10.0f, -96.0f / 20.0f), 1e-6);
  const float wet = kC * std::pow(10.0f, -6.0f / 20.0f);
  SND_CHECK_EQ(r.ArgMaxAbs(f0 + 100, f0 + 5000), f0 + 3600);
  SND_CHECK_NEAR(r.h.At(f0 + 3600, 0), wet, 2e-3);
  SND_CHECK_NEAR(r.h.At(f0 + 7200, 0) / r.h.At(f0 + 3600, 0), 0.5, 2e-3);
  SND_CHECK_NEAR(r.h.At(f0 + 10800, 0) / r.h.At(f0 + 7200, 0), 0.5, 2e-3);
}

SND_TEST(IndepG2Kinds, DelayPingPongAlternatesSides) {
  Rig r({TEffect{dsp::kEffectDelay, {50.0f, 0.7f, -96.0f, 0.0f, 20000.0f, 1.0f, 100.0f}}});
  r.AddSig("Imp", 3e6f, 2);  // stereo voice: impulse on the left only
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("Imp", 60);
  double diff = 0;
  for (int k = 1; k <= 4; ++k) {
    const std::size_t at = f0 + 2400u * std::size_t(k);
    diff = std::fmax(diff, std::fabs(r.MaxAbs(at - 4, at + 4, 0) - r.MaxAbs(at - 4, at + 4, 1)));
  }
  SND_CHECK_MSG(diff > 0.05, "ping-pong repeats are equal on both sides (largest difference %g)", diff);
}

// --- Chorus -------------------------------------------------------------------------
SND_TEST(IndepG2Kinds, ChorusWithoutModulationIsAFixedDelay) {
  Rig r({TEffect{dsp::kEffectChorus, {0.0f, 1.0f, 12.0f, 0.0f, 0.0f, 0.0f}}});
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("Imp", 10);
  SND_CHECK(std::fabs(r.h.At(f0, 0)) < 1e-6f);  // DryMix 0
  SND_CHECK_EQ(r.ArgMaxAbs(f0, f0 + 2000), f0 + 576);
  // Depth 1, Rate 2 Hz: a 2 kHz tone's pitch wobbles: the energy spreads off 2 kHz.
  Rig m({TEffect{dsp::kEffectChorus, {0.0f, 1.0f, 12.0f, 2.0f, 1.0f, 0.0f}}});
  Rig s({TEffect{dsp::kEffectChorus, {0.0f, 1.0f, 12.0f, 0.0f, 0.0f, 0.0f}}});
  m.AddSine("S", 2000.0f);
  s.AddSine("S", 2000.0f);
  SND_REQUIRE(m.Load() && s.Load());
  const std::size_t a = m.PlayAndRender("S", 200), b = s.PlayAndRender("S", 200);
  const double am = m.Amp(a + 9600, 32768, 2000.0), as = s.Amp(b + 9600, 32768, 2000.0);
  std::printf("  chorus 2 kHz: static %.4f, modulated %.4f\n", as, am);
  SND_CHECK(am < 0.8 * as);
}

// --- Flanger ------------------------------------------------------------------------
SND_TEST(IndepG2Kinds, FlangerCombNotchesAndMixes) {
  // Depth 0: a fixed 2 ms comb, dry 0.5 + wet 0.5: notches at 250, 750 Hz, peak at 500 Hz.
  Rig r({TEffect{dsp::kEffectFlanger, {0.5f, 0.5f, 2.0f, 0.0f, 0.0f, 0.0f}}});
  r.AddSine("S250", 250.0f);
  r.AddSine("S500", 500.0f);
  SND_REQUIRE(r.Load());
  const std::size_t a = r.PlayAndRender("S250", 60);
  const double n250 = r.Amp(a + 4800, 8192, 250.0);
  r.h.Steps(1);
  r.Play("S500");
  r.h.Steps(60);
  const std::size_t b = r.h.Total() - 8192 - 256;
  const double p500 = r.Amp(b, 8192, 500.0);
  std::printf("  flanger comb: 250 Hz %.4f, 500 Hz %.4f (of %.4f)\n", n250, p500, kC);
  SND_CHECK(n250 < 0.02 * kC);
  SND_CHECK_NEAR(p500, kC, 0.02);
  // WetMix 0 at run time: dry 0.45 only.
  Rig d({TEffect{dsp::kEffectFlanger, {0.45f, 0.55f, 1.0f, 0.1f, 1.0f, 0.5f}}});
  d.AddImpulse();
  SND_REQUIRE(d.Load());
  d.h.sys->SetEffectParam(HashId("Sfx"), 0, 1, 0.0f, 0.0f);
  d.h.Steps(2);
  const std::size_t f0 = d.PlayAndRender("Imp", 20);
  SND_CHECK_NEAR(d.h.At(f0, 0), 0.45f * kC, 1e-5);
  SND_CHECK(d.MaxAbs(f0 + 1, f0 + 4000) < 1e-6f);
}

// --- Distortion ---------------------------------------------------------------------
SND_TEST(IndepG2Kinds, DistortionClipsOutputGainAndMix) {
  Rig r({TEffect{dsp::kEffectDistortion, {1.0f, 1.0f, 0.0f, 20000.0f}}});
  r.AddSig("Lo", 2000.0f + 48000.0f);  // 1 s of DC 1.0 at a quarter volume (below)
  r.AddSig("Hi", 2000.0f + 480000.0f);
  SND_REQUIRE(r.Load());
  const snd_playback_handle lp = r.Play("Lo");
  r.h.sys->SetParamPlayback(lp, 0xFFFFFFFDu, 0.25f);  // kParamPlaybackVolume (D-104)
  r.h.Steps(8);
  const float lo = r.h.At(r.h.Total() - 1, 0);
  r.h.Steps(200);  // Lo ends (1 s)
  r.PlayAndRender("Hi", 8);
  const float hi = r.h.At(r.h.Total() - 1, 0);
  std::printf("  distortion drive 1: in %.3f -> %.4f, in %.3f -> %.4f\n", 0.25f * kC, lo, kC, hi);
  SND_CHECK(hi <= 1.0f);
  SND_CHECK(hi / lo < 0.8f * 4.0f);  // compresses: 4x the input, much less than 4x the output
  // OutputDb -12 at run time: exactly 10^(-12/20) of the output.
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, -12.0f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.At(r.h.Total() - 1, 0) / hi, std::pow(10.0, -12.0 / 20.0), 1e-3);
  // Mix 0 (OutputDb back to 0): the input as it is.
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, 0.0f);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 1, 0.0f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.At(r.h.Total() - 1, 0), kC, 1e-5);
}

// --- Compressor ---------------------------------------------------------------------
SND_TEST(IndepG2Kinds, CompressorThresholdRatioMakeupAndAttack) {
  // DC 0.9 at the centre pan: -3.92 dB. Threshold -30, ratio 3, knee 0: output -21.31 dB.
  Rig r({TEffect{dsp::kEffectCompressor, {-30.0f, 3.0f, 20.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}}});
  r.AddSig("D", 900.0f);  // DC 0.9
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("D", 60);
  const double in = Db(0.9 * kC);
  const double outDb = -30.0 + (in + 30.0) / 3.0;
  SND_CHECK_NEAR(Db(r.h.At(r.h.Total() - 1, 0)), outDb, 0.3);
  // Attack 20 ms: the remaining gain change in dB falls to 1/e after 960 frames.
  const double g0 = 0.0, gEnd = outDb - in;
  const double g960 = Db(r.h.At(f0 + 960, 0) / (0.9 * kC));
  const double remaining = (g960 - gEnd) / (g0 - gEnd);
  std::printf("  attack: remaining change after 20 ms %.3f (1/e = 0.368)\n", remaining);
  SND_CHECK_NEAR(remaining, 0.3679, 0.06);
  // Makeup +6 dB, then threshold -10 at run time: -10 + 6.08 / 3 = -7.97 dB (+6).
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 5, 6.0f, 0.0f);
  r.h.Steps(20);
  SND_CHECK_NEAR(Db(r.h.At(r.h.Total() - 1, 0)), outDb + 6.0, 0.3);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 0, -10.0f, 0.0f);
  r.h.Steps(40);
  SND_CHECK_NEAR(Db(r.h.At(r.h.Total() - 1, 0)), -10.0 + (in + 10.0) / 3.0 + 6.0, 0.3);
}

// --- Limiter ------------------------------------------------------------------------
SND_TEST(IndepG2Kinds, LimiterCeilingAndLookAheadLatency) {
  // Input gain +12 dB on a 0.9 sine at the centre: 2.54 peak; ceiling -6 dB = 0.501.
  Rig r({TEffect{dsp::kEffectLimiter, {-6.0f, 50.0f, 5.0f, 12.0f}}});
  r.AddSine("S", 200.0f);
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("S", 60);
  const float ceiling = std::pow(10.0f, -6.0f / 20.0f);
  const float peak = r.MaxAbs(f0, r.h.Total());
  std::printf("  limiter peak %.5f (ceiling %.5f), steady %.5f\n", peak, ceiling, r.MaxAbs(f0 + 9600, r.h.Total()));
  SND_CHECK(peak <= ceiling * 1.0005f);  // brickwall: also at the onset (look-ahead)
  SND_CHECK(r.MaxAbs(f0 + 9600, r.h.Total()) > 0.9f * ceiling);
  // Latency = look-ahead - 1 frame (effects.hpp): a quiet impulse (input gain 0) comes out at 239.
  Rig q({TEffect{dsp::kEffectLimiter, {-6.0f, 50.0f, 5.0f, 0.0f}}});
  q.AddImpulse("I", 0.1f);
  SND_REQUIRE(q.Load());
  const std::size_t f1 = q.PlayAndRender("I", 6);
  SND_CHECK_EQ(q.ArgMaxAbs(f1, f1 + 1000), f1 + 239);
  SND_CHECK_NEAR(q.h.At(f1 + 239, 0), 0.1f * kC, 1e-5);
}

// --- ParamEq (D-135 band types) -----------------------------------------------------
namespace {
// Gain (dB) of the bus for a sine of `hz` through a ParamEq with band 0 = (type, f, q, gain).
double EqGain(float type, float f, float q, float gainDb, float hz) {
  std::vector<float> p = {type, f, q, gainDb, 0.0f, 500.0f, 0.707f, 0.0f, 0.0f, 2000.0f, 0.707f, 0.0f,
                          0.0f, 8000.0f, 0.707f, 0.0f};
  Rig r({TEffect{dsp::kEffectParamEq, p}});
  r.AddSine("S", hz);
  if (!r.Load()) return 999.0;
  const std::size_t f0 = r.PlayAndRender("S", 80);
  return Db(r.Amp(f0 + 9600, 8192, hz) / kC);
}
}  // namespace

SND_TEST(IndepG2Kinds, ParamEqBandTypesFrequencyResponse) {
  struct Case {
    float type, f, q, g, hz, expDb, tol;
  };
  const Case cs[] = {
      {1, 1000, 2, 12, 1000, 12.0f, 0.5f},   {1, 1000, 2, 12, 100, 0.0f, 0.5f},    // peaking
      {2, 300, 0.707f, 6, 50, 6.0f, 0.8f},   {2, 300, 0.707f, 6, 6000, 0.0f, 0.5f},  // low shelf
      {3, 3000, 0.707f, -9, 15000, -9.0f, 1.0f}, {3, 3000, 0.707f, -9, 200, 0.0f, 0.5f},  // high shelf
      {4, 1000, 0.707f, 0, 1000, -3.01f, 0.5f}, {4, 1000, 0.707f, 0, 4000, -24.0f, 2.0f},  // LP
      {5, 1000, 0.707f, 0, 1000, -3.01f, 0.5f}, {5, 1000, 0.707f, 0, 250, -24.0f, 2.0f},   // HP
      {6, 1000, 2, 0, 1000, 0.0f, 0.5f},     {6, 1000, 2, 0, 100, -26.0f, 1.0f},   // BP (0 dB peak): (f/f0)/Q
      {8, 1000, 1, 0, 300, 0.0f, 0.1f},      {8, 1000, 1, 0, 1000, 0.0f, 0.1f},    // allpass
      {8, 1000, 1, 0, 5000, 0.0f, 0.1f},
  };
  for (const Case& c : cs) {
    const double g = EqGain(c.type, c.f, c.q, c.g, c.hz);
    SND_CHECK_MSG(std::fabs(g - c.expDb) <= c.tol, "type %.0f f %.0f q %.2f gain %.0f at %.0f Hz: %.2f dB, expected %.2f",
                  c.type, c.f, c.q, c.g, c.hz, g, c.expDb);
  }
  // Notch: deep at the centre, flat away.
  SND_CHECK(EqGain(7, 2000, 5, 0, 2000) < -30.0);
  SND_CHECK_NEAR(EqGain(7, 2000, 5, 0, 500), 0.0, 0.5);
}

SND_TEST(IndepG2Kinds, ParamEqGainChangedAtRunTime) {
  std::vector<float> p = {1, 1000, 2, 0, 0, 500, 0.707f, 0, 0, 2000, 0.707f, 0, 0, 8000, 0.707f, 0};
  Rig r({TEffect{dsp::kEffectParamEq, p}});
  r.AddSine("S", 1000.0f);
  SND_REQUIRE(r.Load());
  r.PlayAndRender("S", 20);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 3, -18.0f, 0.05f);
  r.h.Steps(60);
  SND_CHECK_NEAR(Db(r.Amp(r.h.Total() - 8192 - 64, 8192, 1000.0) / kC), -18.0, 0.5);
}

// --- Lowpass / Highpass -------------------------------------------------------------
SND_TEST(IndepG2Kinds, LowpassHighpassCutoffAndSlope) {
  auto gain = [](snd::Id kind, float cutoff, float q, float hz) {
    Rig r({TEffect{kind, {cutoff, q}}});
    r.AddSine("S", hz);
    if (!r.Load()) return 999.0;
    const std::size_t f0 = r.PlayAndRender("S", 80);
    return Db(r.Amp(f0 + 9600, 8192, hz) / kC);
  };
  SND_CHECK_NEAR(gain(dsp::kEffectLowpass, 2000, 0.707f, 2000), -3.01, 0.5);
  // 12 dB/oct, bilinear: |H| = 1 / sqrt(1 + w^4), w = tan(pi f / fs) / tan(pi fc / fs) = 4.38: -25.7 dB
  SND_CHECK_NEAR(gain(dsp::kEffectLowpass, 2000, 0.707f, 8000), -25.7, 0.7);
  SND_CHECK_NEAR(gain(dsp::kEffectLowpass, 2000, 0.707f, 200), 0.0, 0.2);
  SND_CHECK_NEAR(gain(dsp::kEffectLowpass, 2000, 4.0f, 2000), Db(4.0), 0.7);  // resonance = Q at the cutoff
  SND_CHECK_NEAR(gain(dsp::kEffectHighpass, 500, 0.707f, 500), -3.01, 0.5);
  SND_CHECK_NEAR(gain(dsp::kEffectHighpass, 500, 0.707f, 125), -24.1, 1.5);
  SND_CHECK_NEAR(gain(dsp::kEffectHighpass, 500, 0.707f, 5000), 0.0, 0.2);
  // Cutoff moved at run time.
  Rig r({TEffect{dsp::kEffectLowpass, {20000.0f, 0.707f}}});
  r.AddSine("S", 4000.0f);
  SND_REQUIRE(r.Load());
  r.PlayAndRender("S", 20);
  SND_CHECK_NEAR(Db(r.Amp(r.h.Total() - 4096, 4096, 4000.0) / kC), 0.0, 0.3);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 1000.0f, 0.02f);
  r.h.Steps(40);
  SND_CHECK_NEAR(Db(r.Amp(r.h.Total() - 8192, 8192, 4000.0) / kC), -24.5, 1.0);  // w = 4.09
}

// --- Pitch shifter ------------------------------------------------------------------
SND_TEST(IndepG2Kinds, PitchShifterRatioAndLatency) {
  for (const float ratio : {2.0f, 0.5f}) {
    Rig r({TEffect{dsp::kEffectPitchShifter, {ratio, 40.0f, 1.0f}}});
    r.AddSine("S", 600.0f);
    SND_REQUIRE(r.Load());
    const std::size_t f0 = r.PlayAndRender("S", 100);
    const double shifted = r.Amp(f0 + 9600, 16384, 600.0 * ratio), orig = r.Amp(f0 + 9600, 16384, 600.0);
    std::printf("  pitch %.1f: %.0f Hz %.4f, 600 Hz %.4f\n", ratio, 600.0 * ratio, shifted, orig);
    SND_CHECK(shifted > 0.3 * kC);
    SND_CHECK(orig < 0.25 * shifted);
  }
  Rig r({TEffect{dsp::kEffectPitchShifter, {1.0f, 30.0f, 1.0f}}});
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  const std::size_t f0 = r.PlayAndRender("Imp", 20);
  SND_CHECK_EQ(r.ArgMaxAbs(f0, f0 + 4000), f0 + 720);  // WindowMs / 2 = 15 ms
}

// --- Normalize ----------------------------------------------------------------------
SND_TEST(IndepG2Kinds, NormalizeRaisesAQuietSignalWithinMaximumAmp) {
  Rig r({TEffect{dsp::kEffectNormalize, {0.0f, 0.1f, 20.0f}}});
  r.AddSine("S", 300.0f);
  SND_REQUIRE(r.Load());
  // A quiet sine: clip at -16 dB (0.158 * 0.707 = 0.112 peak at the bus, above LowestVolume 0.1).
  const snd_playback_handle p = r.Play("S");
  r.h.sys->SetParamPlayback(p, 0xFFFFFFFDu, 0.158f);  // kParamPlaybackVolume (D-104)
  r.h.Steps(200);
  const float in = 0.158f * kC;
  const float out = r.MaxAbs(r.h.Total() - 4800, r.h.Total());
  std::printf("  normalize: input peak %.4f -> output peak %.4f\n", in, out);
  SND_CHECK(out > 3.0f * in);
  SND_CHECK(out <= 20.0f * in * 1.001f);
  SND_CHECK(out <= 1.001f);
}

// --- Surround -----------------------------------------------------------------------
SND_TEST(IndepG2Kinds, SurroundWidthOnStereo) {
  auto run = [&](float width, double* l300, double* l700, double* r700) {
    Rig r({TEffect{dsp::kEffectSurround, {width, -6.0f, 15.0f, 7000.0f, -60.0f}}});
    r.AddSig("St", 2e6f, 2);  // L sine 300 Hz, R sine 700 Hz, 0.5
    if (!r.Load()) return;
    const std::size_t f0 = r.PlayAndRender("St", 40);
    *l300 = r.Amp(f0 + 2400, 4096, 300.0, 0);
    *l700 = r.Amp(f0 + 2400, 4096, 700.0, 0);
    *r700 = r.Amp(f0 + 2400, 4096, 700.0, 1);
  };
  double a[3], b[3], c[3];
  run(1.0f, &a[0], &a[1], &a[2]);
  run(0.0f, &b[0], &b[1], &b[2]);
  run(2.0f, &c[0], &c[1], &c[2]);
  std::printf("  width 1: L300 %.3f L700 %.3f R700 %.3f | 0: %.3f %.3f %.3f | 2: %.3f %.3f %.3f\n", a[0], a[1], a[2], b[0],
              b[1], b[2], c[0], c[1], c[2]);
  SND_CHECK(a[1] < 0.02);                   // width 1: L has no 700 Hz (unchanged)
  SND_CHECK_NEAR(b[1], b[2], 0.01);         // width 0: mono
  SND_CHECK(c[1] > 0.05);                    // width 2: wider: the other side shows up inverted
}

SND_TEST(IndepG2Kinds, SurroundFeedsTheRearsOfA51Bus) {
  Rig r({TEffect{dsp::kEffectSurround, {1.0f, -6.0f, 15.0f, 20000.0f, -60.0f}}}, 6, 6);
  r.AddImpulse();
  SND_REQUIRE(r.Load());
  SND_REQUIRE_EQ(r.h.Ch(), 6u);
  const std::size_t f0 = r.PlayAndRender("Imp", 10);
  const float front = r.MaxAbs(f0, f0 + 100, 0);
  SND_REQUIRE(front > 0.1f);
  for (std::uint32_t c : {4u, 5u}) {
    const std::size_t at = r.ArgMaxAbs(f0, f0 + 2000, c);
    std::printf("  rear %u: peak %.4f at +%llu (front %.4f)\n", c, r.h.At(at, c), static_cast<unsigned long long>(at - f0), front);
    SND_CHECK_NEAR(double(at - f0), 720.0, 3.0);
    SND_CHECK(r.MaxAbs(f0, f0 + 700, c) < 1e-3f * front);
  }
}

// --- Extreme values on mono / stereo / 5.1 buses (D-14) -----------------------------
SND_TEST(IndepG2Kinds, ExtremeValuesStayFiniteAndBounded) {
  const snd::Id kinds[] = {dsp::kEffectReverb,     dsp::kEffectEcho,      dsp::kEffectDelay,    dsp::kEffectChorus,
                           dsp::kEffectFlanger,    dsp::kEffectDistortion, dsp::kEffectCompressor, dsp::kEffectLimiter,
                           dsp::kEffectParamEq,    dsp::kEffectLowpass,   dsp::kEffectHighpass, dsp::kEffectPitchShifter,
                           dsp::kEffectNormalize,  dsp::kEffectSurround};
  const float extremes[] = {1e30f, -1e30f, 0.0f, 1e-30f, -1.0f, 25000.0f};
  for (const std::uint32_t ch : {1u, 2u, 6u}) {
    for (const snd::Id k : kinds) {
      const dsp::EffectInfo* info = dsp::FindEffect(k);
      SND_REQUIRE(info != nullptr);
      // Bank values: NaN / +-Inf (runtime clamps: NaN -> default, Inf -> bound).
      std::vector<float> bankParams(info->paramCount);
      for (std::uint32_t i = 0; i < info->paramCount; ++i) {
        bankParams[i] = i % 3 == 0 ? std::nanf("") : (i % 3 == 1 ? INFINITY : -INFINITY);
      }
      Rig r({TEffect{k, bankParams}}, ch, ch == 6u ? 6u : 2u);
      r.AddSine("S", 440.0f);
      r.AddImpulse();
      SND_REQUIRE(r.Load());
      r.Play("S");
      r.Play("Imp");
      r.h.Steps(8);
      for (std::uint32_t i = 0; i < info->paramCount; ++i) {
        for (const float v : extremes) {
          r.h.sys->SetEffectParam(HashId("Sfx"), 0, i, v, (i % 2) != 0 ? 0.0f : 0.01f);
          r.h.Steps(1);
        }
      }
      r.h.Steps(20);
      bool finite = true;
      float peak = 0;
      for (float x : r.h.all) {
        finite = finite && std::isfinite(x);
        peak = std::fmax(peak, std::fabs(x));
      }
      SND_CHECK_MSG(finite && peak < 100.0f, "%s on a %u-channel bus: finite %d, peak %g", info->name, ch, finite ? 1 : 0,
                    peak);
    }
  }
}

// --- Standard per-voice lowpass / highpass ----------------------------------------------
SND_TEST(IndepG2VoiceFilter, ClipRandomFilterVariesPerPlayAndIsDeterministic) {
  auto run = [] {
    Rig r({});
    r.AddSine("S", 6000.0f);
    r.b.events.back().tracks[0].clips[0].randFilterHz = 18000.0f;
    std::vector<double> amps;
    if (!r.Load()) return amps;
    for (int i = 0; i < 8; ++i) {
      const snd_playback_handle p = r.Play("S");
      r.h.Steps(20);
      amps.push_back(r.Amp(r.h.Total() - 4096, 4096, 6000.0) / kC);
      r.h.sys->Stop(p, 0.0f, nullptr);
      r.h.Steps(4);
    }
    return amps;
  };
  const std::vector<double> a = run(), b = run();
  SND_REQUIRE_EQ(a.size(), 8u);
  double lo = 1e9, hi = 0;
  for (double x : a) {
    lo = std::fmin(lo, x);
    hi = std::fmax(hi, x);
  }
  std::printf("  random filter at 6 kHz: %.3f .. %.3f\n", lo, hi);
  SND_CHECK(hi <= 1.001);
  SND_CHECK(hi - lo > 0.05);  // the cutoff varies from play to play
  SND_CHECK(lo < 0.9);
  SND_CHECK(a == b);
}

SND_TEST(IndepG2VoiceFilter, ParameterCurveDrivesLowpassAndHighpass) {
  for (int hp = 0; hp < 2; ++hp) {
    Rig r({});
    r.AddSine("S", hp != 0 ? 200.0f : 6000.0f);
    r.b.params.push_back(TParam{HashId("X"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
    TCurve c;
    c.input = HashId("X");
    c.target = static_cast<std::uint32_t>(hp != 0 ? bf::CurveTarget::Highpass : bf::CurveTarget::Lowpass);
    c.compose = static_cast<std::uint32_t>(hp != 0 ? bf::CurveCompose::Max : bf::CurveCompose::Min);
    c.points = hp != 0 ? std::vector<std::pair<float, float>>{{0.0f, 10.0f}, {1.0f, 2000.0f}}
                       : std::vector<std::pair<float, float>>{{0.0f, 22000.0f}, {1.0f, 600.0f}};
    r.b.curves.push_back(c);
    r.b.events.back().curves = {0};
    SND_REQUIRE(r.Load());
    const double hz = hp != 0 ? 200.0 : 6000.0;
    r.Play("S");
    r.h.Steps(20);
    const double open = r.Amp(r.h.Total() - 4096, 4096, hz) / kC;
    r.h.sys->SetParamGlobal(HashId("X"), 1.0f);
    r.h.Steps(20);
    const double closed = r.Amp(r.h.Total() - 4096, 4096, hz) / kC;
    std::printf("  %s curve: open %.3f, closed %.4f\n", hp != 0 ? "highpass" : "lowpass", open, closed);
    SND_CHECK(open > 0.95);
    SND_CHECK(closed < 0.1);  // a decade away from the cutoff, 12 dB/oct x2 (filter.hpp): far below -20 dB
  }
}

SND_TEST(IndepG2VoiceFilter, DistanceCurveLowpass) {
  auto ratioAt = [](float distance) {
    Rig r({});
    r.AddSine("Hi", 6000.0f);
    r.AddSine("Lo", 150.0f);
    r.b.curves.push_back(TCurve{bf::kInputDistance, static_cast<std::uint32_t>(bf::CurveTarget::Lowpass),
                                static_cast<std::uint32_t>(bf::CurveCompose::Min), {{0.0f, 22000.0f}, {50.0f, 800.0f}}});
    for (TEvent& e : r.b.events) {
      e.flags = bf::kEvent3D;
      e.curveLowpass = 0;
      e.maxDistance = 100.0f;
    }
    if (!r.Load()) return -1.0;
    snd_listener_desc l{};
    l.forward = snd_vec3{0, 0, 1};
    l.up = snd_vec3{0, 1, 0};
    l.weight = 1.0f;
    l.enabled = 1u;
    r.h.sys->SetListener(0, &l);
    const snd_emitter_handle em = r.h.sys->CreateEmitter();
    const snd_vec3 pos{0.0f, 0.0f, distance}, fwd{0, 0, 1}, vel{0, 0, 0};
    r.h.sys->SetTransform(em, &pos, &fwd, &vel);
    r.h.Steps(4);
    r.h.sys->Play(HashId("Hi"), em, nullptr);
    r.h.sys->Play(HashId("Lo"), em, nullptr);
    for (int i = 0; i < 80; ++i) {  // the emitter stays put (its smoothing is 3D's business, not this test's)
      r.h.sys->SetTransform(em, &pos, &fwd, &vel);
      r.h.Step();
    }
    const std::size_t from = r.h.Total() - 8192;
    const double hi = r.Amp(from, 8192, 6000.0) + r.Amp(from, 8192, 6000.0, 1);
    const double lo = r.Amp(from, 8192, 150.0) + r.Amp(from, 8192, 150.0, 1);
    return lo > 0 ? hi / lo : -1.0;
  };
  const double nearR = ratioAt(1.0f), farR = ratioAt(50.0f);
  std::printf("  6 kHz / 150 Hz: at 1 m %.3f, at 50 m %.4f\n", nearR, farR);
  SND_CHECK_NEAR(nearR, 1.0, 0.05);
  SND_CHECK(farR < 0.05);
}
