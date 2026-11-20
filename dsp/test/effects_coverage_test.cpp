// Stage 3 group 2 (dsp): "各種フィルタ" as the parametric EQ band types (D-135,
// allpass appended) checked against the analytic biquad magnitude, and the I3DL2
// reverb parameter set doing what each parameter is for (RT60 follows DecayTime,
// levels, delays, filters, density). Expectations are analytic or relative
// (same input, one parameter changed); no golden audio.
#include <cmath>
#include <cstring>
#include <vector>

#include "dsp_test_util.hpp"
#include "indep_fx_util.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using indepfx::Fx;

namespace {
constexpr double kSr = 48000.0;

// Runs `in` (mono) through the instance in blocks of 256, returns the output.
std::vector<float> RunMono(Fx& fx, const std::vector<float>& in) {
  std::vector<float> out(in.size(), 0.0f);
  for (std::size_t at = 0; at < in.size(); at += 256) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(256, in.size() - at));
    const float* i[1] = {in.data() + at};
    float* o[1] = {out.data() + at};
    EffectProcess(fx.state(), i, o, 1, n, nullptr, nullptr);
  }
  return out;
}
std::vector<float> SineV(std::size_t n, double hz, float amp = 0.5f) {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2.0 * dsptest::kPi * hz * double(i) / kSr));
  return v;
}
double ToneAt(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
  return dsptest::ToneAmplitude(x.data() + from, static_cast<std::uint32_t>(n), hz, kSr);
}
}  // namespace

// ---------------------------------------------------------------------------
// D-135: band types of snd.fx.parameq
// ---------------------------------------------------------------------------
SND_TEST(EffectsCoverage, ParamEqBandTypesFollowTheirMagnitudeResponse) {
  struct Case {
    float type;
    BiquadType bq;
    const char* name;
  };
  const Case cases[] = {{1, BiquadType::Peaking, "peaking"},  {2, BiquadType::LowShelf, "low shelf"},
                        {3, BiquadType::HighShelf, "high shelf"}, {4, BiquadType::Lowpass, "lowpass"},
                        {5, BiquadType::Highpass, "highpass"}, {6, BiquadType::Bandpass, "bandpass"},
                        {7, BiquadType::Notch, "notch"},      {8, BiquadType::Allpass, "allpass"}};
  const double freqs[] = {60.0, 300.0, 1000.0, 3000.0, 12000.0};
  for (const Case& c : cases) {
    // Band 1 only (the others off): f0 1 kHz, Q 1, +12 dB.
    Fx fx(EffectKind::ParamEq, 1, 256, 48000,
          {{0, c.type}, {1, 1000.0f}, {2, 1.0f}, {3, 12.0f}, {4, 0.0f}, {8, 0.0f}, {12, 0.0f}});
    SND_REQUIRE(fx.ok);
    const BiquadCoefs ref = DesignBiquad(c.bq, 1000.0f, 1.0f, 12.0f, 48000.0f);
    for (double f : freqs) {
      Fx fresh(EffectKind::ParamEq, 1, 256, 48000,
               {{0, c.type}, {1, 1000.0f}, {2, 1.0f}, {3, 12.0f}, {4, 0.0f}, {8, 0.0f}, {12, 0.0f}});
      const std::vector<float> out = RunMono(fresh, SineV(48000, f));
      const double got = dsptest::Db(ToneAt(out, 24000, 16384, f) / 0.5);
      const double want = dsptest::Db(BiquadMagnitude(ref, f, kSr));
      SND_CHECK_MSG(std::fabs(got - want) < (want < -40.0 ? 3.0 : 0.15), "%s at %.0f Hz: %.2f dB, analytic %.2f dB",
                    c.name, f, got, want);
    }
  }
  // What each type is for (the analytic curve above is the reference; these are its landmarks).
  auto at = [](float type, double f) {
    Fx fx(EffectKind::ParamEq, 1, 256, 48000,
          {{0, type}, {1, 1000.0f}, {2, 1.0f}, {3, 12.0f}, {4, 0.0f}, {8, 0.0f}, {12, 0.0f}});
    const std::vector<float> out = RunMono(fx, SineV(48000, f));
    return dsptest::Db(ToneAt(out, 24000, 16384, f) / 0.5);
  };
  SND_CHECK_NEAR(at(1, 1000.0), 12.0, 0.2);   // peaking: +gain at f0
  SND_CHECK_NEAR(at(1, 60.0), 0.0, 0.3);
  SND_CHECK_NEAR(at(2, 60.0), 12.0, 0.4);     // low shelf: +gain below
  SND_CHECK_NEAR(at(2, 12000.0), 0.0, 0.2);
  SND_CHECK_NEAR(at(3, 12000.0), 12.0, 0.4);  // high shelf: +gain above
  SND_CHECK_NEAR(at(3, 60.0), 0.0, 0.2);
  SND_CHECK(at(4, 12000.0) < -38.0);          // lowpass: 12 dB/oct above f0
  SND_CHECK(at(5, 60.0) < -45.0);             // highpass: 12 dB/oct below f0
  SND_CHECK_NEAR(at(6, 1000.0), 0.0, 0.2);    // bandpass: 0 dB at f0 ...
  SND_CHECK(at(6, 60.0) < -20.0 && at(6, 12000.0) < -20.0);  // ... falls away on both sides
  SND_CHECK(at(7, 1000.0) < -30.0);           // notch: deep at f0 ...
  SND_CHECK_NEAR(at(7, 60.0), 0.0, 0.2);      // ... flat away from it
  for (double f : freqs) SND_CHECK_NEAR(at(8, f), 0.0, 0.1);  // allpass: flat magnitude
  // Allpass: the phase turns by 180 degrees at f0 (output = -input in steady state).
  {
    Fx fx(EffectKind::ParamEq, 1, 256, 48000, {{0, 8.0f}, {1, 1000.0f}, {2, 1.0f}, {4, 0.0f}, {8, 0.0f}, {12, 0.0f}});
    const std::vector<float> in = SineV(48000, 1000.0);
    const std::vector<float> out = RunMono(fx, in);
    double dot = 0, nn = 0;
    for (std::size_t i = 24000; i < 48000; ++i) {
      dot += double(in[i]) * out[i];
      nn += double(in[i]) * in[i];
    }
    SND_CHECK_NEAR(dot / nn, -1.0, 0.01);
  }
}

// ---------------------------------------------------------------------------
// I3DL2 reverb: every parameter does what it is for
// ---------------------------------------------------------------------------
namespace {
// Reverb with only the late path (dry and reflections off) unless overridden.
Fx Reverb(std::vector<std::pair<std::uint32_t, float>> p) {
  std::vector<std::pair<std::uint32_t, float>> all = {{0, -10000.0f}, {1, 0.0f}, {2, 0.0f}, {5, 1.0f},
                                                      {6, -10000.0f}, {8, 0.0f}};
  for (const auto& kv : p) all.push_back(kv);
  return Fx(EffectKind::Reverb, 1, 256, 48000, all);
}
std::vector<float> ImpulseResponse(Fx& fx, std::size_t frames) {
  std::vector<float> in(frames, 0.0f);
  in[0] = 1.0f;
  return RunMono(fx, in);
}
// RT60 from the Schroeder backward integral (fit between -5 and -35 dB).
double Rt60(const std::vector<float>& h) {
  std::vector<double> e(h.size() + 1, 0.0);
  for (std::size_t i = h.size(); i-- > 0;) e[i] = e[i + 1] + double(h[i]) * h[i];
  const double total = e[0];
  std::size_t a = 0, b = 0;
  for (std::size_t i = 0; i < h.size(); ++i) {
    const double db = 10.0 * std::log10(e[i] / total + 1e-300);
    if (a == 0 && db < -5.0) a = i;
    if (b == 0 && db < -35.0) {
      b = i;
      break;
    }
  }
  if (a == 0 || b <= a) return 0.0;
  return 60.0 / (30.0 / (double(b - a) / kSr));
}
std::size_t FirstAbove(const std::vector<float>& x, float eps) {
  for (std::size_t i = 0; i < x.size(); ++i)
    if (std::fabs(x[i]) > eps) return i;
  return x.size();
}
double Energy(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  double s = 0;
  for (std::size_t i = from; i < x.size() && i < to; ++i) s += double(x[i]) * x[i];
  return s;
}
}  // namespace

SND_TEST(EffectsCoverage, ReverbDecayTimeSetsTheRt60) {
  for (float t60 : {0.5f, 1.0f, 2.0f, 4.0f}) {
    Fx fx = Reverb({{4, t60}});
    SND_REQUIRE(fx.ok);
    const std::vector<float> h = ImpulseResponse(fx, static_cast<std::size_t>(kSr * (t60 * 1.5 + 0.5)));
    const double rt = Rt60(h);
    snd::test::Note("DecayTime %.2f s: RT60 %.3f s", t60, rt);
    SND_CHECK(std::fabs(rt - t60) < 0.15 * t60);
  }
}

SND_TEST(EffectsCoverage, ReverbDecayHfRatioShortensTheHighFrequencyDecay) {
  // Noise burst; the 8 kHz part decays faster than the 300 Hz part with ratio 0.3,
  // equally with ratio 1.0 (no damping).
  auto ratio = [](float hfRatio) {
    Fx fx = Reverb({{4, 1.5f}, {5, hfRatio}, {12, 2000.0f}});
    std::vector<float> in(96000, 0.0f);
    dsptest::Noise nz(7);
    for (std::size_t i = 0; i < 4800; ++i) in[i] = nz.Next(0.5f);
    const std::vector<float> out = RunMono(fx, in);
    const double hiEarly = ToneAt(out, 9600, 8192, 8000.0), hiLate = ToneAt(out, 48000, 8192, 8000.0);
    const double loEarly = ToneAt(out, 9600, 8192, 300.0), loLate = ToneAt(out, 48000, 8192, 300.0);
    return dsptest::Db(hiLate / hiEarly) - dsptest::Db(loLate / loEarly);
  };
  const double damped = ratio(0.3f), flat = ratio(1.0f);
  snd::test::Note("HF minus LF decay over 0.8 s: ratio 0.3 %.1f dB, ratio 1.0 %.1f dB", damped, flat);
  SND_CHECK(damped < -10.0);
  SND_CHECK(std::fabs(flat) < 6.0);
}

SND_TEST(EffectsCoverage, ReverbLevelsDelaysAndFilters) {
  const std::size_t n = 48000;
  // Room scales the wet signal: -600 mB = -6 dB of energy.
  {
    Fx a = Reverb({{1, 0.0f}}), b = Reverb({{1, -600.0f}});
    const double ea = Energy(ImpulseResponse(a, n)), eb = Energy(ImpulseResponse(b, n));
    SND_CHECK_NEAR(10.0 * std::log10(eb / ea), -6.0, 0.05);  // mB: 1/100 dB
  }
  // Reverb (late level) and Reflections (early level) scale their parts only.
  {
    Fx a = Reverb({{8, 0.0f}}), b = Reverb({{8, -1000.0f}});
    SND_CHECK_NEAR(10.0 * std::log10(Energy(ImpulseResponse(b, n)) / Energy(ImpulseResponse(a, n))), -10.0, 0.05);
    Fx er = Reverb({{6, 0.0f}, {8, -10000.0f}, {7, 0.02f}});
    const std::vector<float> h = ImpulseResponse(er, n);
    // Early reflections only: 8 taps over 17.1 ms from ReflectionsDelay (20 ms), nothing after.
    SND_CHECK(Energy(h) > 0.1);
    // (Reverb -10000 mB is 1e-5 of amplitude, not off: -100 dB is left of the late part.)
    SND_CHECK(Energy(h, static_cast<std::size_t>(0.045 * kSr)) < Energy(h) * 1e-8);
    // ReflectionsDelay: the first reflection at 20 ms.
    SND_CHECK_NEAR(double(FirstAbove(h, 1e-6f)), 0.02 * kSr, 2.0);
  }
  // ReverbDelay moves the late onset by itself; Density shortens the delay lines.
  {
    Fx a = Reverb({{7, 0.0f}, {9, 0.0f}}), b = Reverb({{7, 0.0f}, {9, 0.05f}});
    // Reflections at -10000 mB still leave 1e-5 of amplitude: the late onset is above 1e-4.
    const std::size_t oa = FirstAbove(ImpulseResponse(a, n), 1e-4f), ob = FirstAbove(ImpulseResponse(b, n), 1e-4f);
    SND_CHECK_NEAR(double(ob) - double(oa), 0.05 * kSr, 3.0);
    Fx d0 = Reverb({{7, 0.0f}, {9, 0.0f}, {11, 0.0f}});
    const std::size_t od = FirstAbove(ImpulseResponse(d0, n), 1e-4f);
    snd::test::Note("late onset: density 100 %zu, density 0 %zu frames", oa, od);
    SND_CHECK(od + 400 < oa);  // shortest line 1031 -> ~516 frames
  }
  // Diffusion: more distinct samples (echo density) early in the late part.
  {
    auto density = [&](float diffusion) {
      Fx fx = Reverb({{10, diffusion}});
      const std::vector<float> h = ImpulseResponse(fx, n);
      const std::size_t o = FirstAbove(h, 1e-4f);
      float peak = 0.0f;
      for (float v : h) peak = std::max(peak, std::fabs(v));
      std::size_t count = 0;
      for (std::size_t i = o; i < o + 960 && i < h.size(); ++i) count += std::fabs(h[i]) > 1e-2f * peak ? 1u : 0u;
      return count;
    };
    const std::size_t sparse = density(0.0f), dense = density(100.0f);
    snd::test::Note("samples above -40 dB of the peak in the first 20 ms of the tail: diffusion 0 %zu, 100 %zu", sparse, dense);
    SND_CHECK(dense > sparse * 2u);
  }
  // RoomHF at HFReference and RoomLF below LFReference (early path, linear: ratios exact).
  {
    auto level = [&](std::vector<std::pair<std::uint32_t, float>> p, double hz) {
      p.push_back({6, 0.0f});
      p.push_back({8, -10000.0f});
      Fx fx = Reverb(p);
      const std::vector<float> out = RunMono(fx, SineV(48000, hz));
      return ToneAt(out, 24000, 16384, hz);
    };
    SND_CHECK_NEAR(dsptest::Db(level({{2, -1200.0f}, {12, 5000.0f}}, 5000.0) / level({{2, 0.0f}, {12, 5000.0f}}, 5000.0)),
                   -12.0, 0.3);
    // HFReference moves where RoomHF acts: at 3 kHz, a 2 kHz reference cuts more than 10 kHz.
    const double cut2k = level({{2, -1200.0f}, {12, 2000.0f}}, 3000.0), cut10k = level({{2, -1200.0f}, {12, 10000.0f}}, 3000.0);
    SND_CHECK(dsptest::Db(cut2k / cut10k) < -4.0);
    const double lf = dsptest::Db(level({{3, -1200.0f}, {13, 250.0f}}, 30.0) / level({{3, 0.0f}, {13, 250.0f}}, 30.0));
    const double hf = dsptest::Db(level({{3, -1200.0f}, {13, 250.0f}}, 8000.0) / level({{3, 0.0f}, {13, 250.0f}}, 8000.0));
    snd::test::Note("RoomLF -12 dB: %.2f dB at 30 Hz, %.2f dB at 8 kHz", lf, hf);
    SND_CHECK_NEAR(lf, -12.0, 2.0);
    SND_CHECK_NEAR(hf, 0.0, 0.5);
  }
  // DryLevel: the dry part alone (wet paths off).
  {
    Fx fx(EffectKind::Reverb, 1, 256, 48000, {{0, -600.0f}, {1, -10000.0f}, {6, -10000.0f}, {8, -10000.0f}});
    const std::vector<float> in = SineV(4800, 440.0);
    const std::vector<float> out = RunMono(fx, in);
    double worst = 0;
    for (std::size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(double(out[i]) - std::pow(10.0, -0.3) * in[i]));
    SND_CHECK(worst < 1e-4);
  }
}
