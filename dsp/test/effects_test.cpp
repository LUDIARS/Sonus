// Standard effects, chains with plugins, pool sizing.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/ext/dsp_effect.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/golden.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;

struct Instance {
  dsptest::AlignedMem mem;
  snd::EffectDesc desc{};
  float params[kMaxEffectParams] = {};
  bool ok = false;
  Instance(EffectKind kind, std::uint32_t channels, std::uint32_t maxFrames = kBlock,
           const float* overrides = nullptr, std::uint32_t overrideCount = 0) {
    const EffectInfo& info = GetEffectInfo(kind);
    EffectDefaultParams(kind, params);
    for (std::uint32_t i = 0; i < overrideCount; ++i) params[i] = overrides[i];
    desc.effect_type = info.typeId;
    desc.channels = channels;
    desc.max_frames = maxFrames;
    desc.param_count = info.paramCount;
    desc.initial_params = params;
    const std::uint32_t bytes = EffectStateBytes(desc, kRate);
    mem.Reset(bytes, dsptest::kDirty);
    ok = bytes > 0 && EffectInit(mem.get(), mem.size(), desc, kRate);
  }
  void* state() const { return mem.get(); }
};

void Process(void* state, Audio& a, std::uint32_t frames, const snd::ParamBlock* pb = nullptr,
             const EffectSidechain* sc = nullptr) {
  SND_RT_SCOPE();
  EffectProcess(state, a.cp(), a.p(), a.channels(), frames, pb, sc);
}

// Stereo test signal: noise burst + two sines.
void TestSignal(Audio& a, std::uint64_t start, std::uint32_t frames) {
  dsptest::Noise nz(static_cast<std::uint32_t>(start / 256u + 11u));
  for (std::uint32_t c = 0; c < a.channels(); ++c) {
    for (std::uint32_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(start + i) / kRate;
      float v = 0.3f * static_cast<float>(std::sin(2 * dsptest::kPi * (220.0 + 110.0 * c) * t)) +
                0.2f * static_cast<float>(std::sin(2 * dsptest::kPi * 3100.0 * t));
      if (start + i < 2000) v += nz.Next(0.4f);
      a.ch(c)[i] = v;
    }
  }
}

const char* ShortName(EffectKind k) {
  const char* n = GetEffectInfo(k).name;
  return n + 7;  // after "snd.fx."
}

}  // namespace

SND_TEST(Effects, RegistryIsConsistent) {
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectInfo& info = GetEffectInfo(static_cast<EffectKind>(k));
    SND_CHECK(FindEffect(info.typeId) == &info);
    SND_CHECK_EQ(info.typeId, snd::HashId(info.name));
    SND_CHECK(info.paramCount > 0 && info.paramCount <= kMaxEffectParams);
    for (std::uint32_t p = 0; p < info.paramCount; ++p) {
      const EffectParamInfo& pi = info.params[p];
      SND_CHECK(pi.minValue <= pi.defaultValue && pi.defaultValue <= pi.maxValue);
    }
    snd::test::Note("%-18s id 0x%08X params %u", info.name, info.typeId, info.paramCount);
  }
  SND_CHECK(FindEffect(snd::HashId("my.plugin")) == nullptr);
}

SND_TEST(Effects, InitSizesAndParams) {
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    for (std::uint32_t ch : {1u, 2u, 6u, 8u}) {
      Instance fx(kind, ch);
      SND_REQUIRE(fx.ok);
      SND_CHECK_EQ(EffectStateBytes(fx.desc, kRate) % 64u, 0u);
      SND_CHECK(EffectStateKind(fx.state()) == kind);
      SND_CHECK(EffectIsTailDone(fx.state()));
      SND_CHECK_EQ(EffectParamValue(fx.state(), 0), fx.params[0]);
      if (ch == 2) snd::test::Note("%-18s stereo state %u bytes", GetEffectInfo(kind).name, EffectStateBytes(fx.desc, kRate));
    }
  }
  // Invalid descs.
  snd::EffectDesc bad{};
  bad.effect_type = kEffectEcho;
  bad.channels = 0;
  bad.max_frames = 256;
  SND_CHECK_EQ(EffectStateBytes(bad, kRate), 0u);
  bad.channels = 9;
  SND_CHECK_EQ(EffectStateBytes(bad, kRate), 0u);
}

// Impulse, then silence without FTZ/DAZ: never NaN/Inf/denormal, and the tail ends.
SND_TEST(Effects, SilenceAfterImpulseIsCleanAndTailEnds) {
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    Instance fx(kind, 6);
    SND_REQUIRE(fx.ok);
    Audio a(6, kBlock);
    a.ch(0)[0] = 1.0f;
    a.ch(1)[3] = -0.7f;
    Process(fx.state(), a, kBlock);
    bool bad = false;
    std::uint32_t blocks = 0;
    const std::uint32_t limit = kRate * 40 / kBlock;
    while (!EffectIsTailDone(fx.state()) && blocks < limit) {
      a.Zero();
      Process(fx.state(), a, kBlock);
      for (std::uint32_t c = 0; c < 6; ++c) bad |= HasNonFinite(a.ch(c), kBlock) || HasDenormal(a.ch(c), kBlock);
      ++blocks;
    }
    SND_CHECK_MSG(!bad, "%s produced NaN/Inf/denormal", ShortName(kind));
    SND_CHECK_MSG(EffectIsTailDone(fx.state()), "%s tail did not end", ShortName(kind));
    snd::test::Note("%-11s tail ended after %.2f s", ShortName(kind),
                    static_cast<double>(blocks) * kBlock / kRate);
    // After the tail, more silence gives exact silence.
    for (int b = 0; b < 8; ++b) {
      a.Zero();
      Process(fx.state(), a, kBlock);
      for (std::uint32_t c = 0; c < 6; ++c) SND_CHECK_EQ(Peak(a.ch(c), kBlock), 0.0f);
    }
    // Denormal input is not amplified into trouble either.
    for (std::uint32_t c = 0; c < 6; ++c)
      for (std::uint32_t i = 0; i < kBlock; ++i) a.ch(c)[i] = 1e-39f;
    Process(fx.state(), a, kBlock);
    for (std::uint32_t c = 0; c < 6; ++c) SND_CHECK(!HasNonFinite(a.ch(c), kBlock));
  }
}

SND_TEST(Effects, BlockSplitInvariance) {
  constexpr std::uint32_t kN = 1024;
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    Instance a(kind, 2, kN), b(kind, 2, kN), c(kind, 2, 64);
    SND_REQUIRE(a.ok && b.ok && c.ok);
    Audio xa(2, kN), xb(2, kN), xc(2, kN);
    TestSignal(xa, 0, kN);
    TestSignal(xb, 0, kN);
    TestSignal(xc, 0, kN);
    Process(a.state(), xa, kN);
    std::uint32_t done = 0;
    for (std::uint32_t n : {256u, 100u, 1u, 155u, 512u}) {
      float* p[2] = {xb.ch(0) + done, xb.ch(1) + done};
      SND_RT_SCOPE();
      EffectProcess(b.state(), p, p, 2, n, nullptr, nullptr);
      done += n;
    }
    SND_REQUIRE_EQ(done, kN);
    Process(c.state(), xc, kN);  // > max_frames: internal sub-blocks
    const float d1 = std::max(dsptest::MaxAbsDiff(xa.ch(0), xb.ch(0), kN), dsptest::MaxAbsDiff(xa.ch(1), xb.ch(1), kN));
    const float d2 = std::max(dsptest::MaxAbsDiff(xa.ch(0), xc.ch(0), kN), dsptest::MaxAbsDiff(xa.ch(1), xc.ch(1), kN));
    SND_CHECK_MSG(d1 == 0.0f && d2 == 0.0f, "%s split diff %g / %g", ShortName(kind), static_cast<double>(d1),
                  static_cast<double>(d2));
  }
}

SND_TEST(Effects, GoldenDefaults) {
  constexpr std::uint32_t kN = 2048;
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    Instance fx(kind, 2);
    SND_REQUIRE(fx.ok);
    Audio out(2, kN);
    Audio blk(2, kBlock);
    for (std::uint32_t b = 0; b < kN / kBlock; ++b) {
      TestSignal(blk, b * kBlock, kBlock);
      Process(fx.state(), blk, kBlock);
      for (std::uint32_t c = 0; c < 2; ++c) std::memcpy(out.ch(c) + b * kBlock, blk.ch(c), kBlock * sizeof(float));
    }
    char name[64];
    std::snprintf(name, sizeof(name), "fx_%s", ShortName(kind));
    snd::test::GoldenTolerance tol;
    tol.maxAbs = 2e-4f;
    tol.maxRms = 2e-5f;
    SND_CHECK_GOLDEN(name, dsptest::ToBuffer(out, kN), tol);
  }
}

SND_TEST(Effects, ParamRampsReachTheEndValue) {
  Instance fx(EffectKind::Distortion, 1);
  SND_REQUIRE(fx.ok);
  float s[4] = {0.0f, 1.0f, 0.0f, 20000.0f};
  float e[4] = {1.0f, 1.0f, -6.0f, 20000.0f};
  snd::ParamBlock pb{s, e, 4, 0x5u, nullptr};
  Audio a(1, kBlock);
  for (std::uint32_t i = 0; i < kBlock; ++i) a.ch(0)[i] = 0.1f;
  Process(fx.state(), a, kBlock, &pb);
  SND_CHECK_EQ(EffectParamValue(fx.state(), 0), 1.0f);
  SND_CHECK_EQ(EffectParamValue(fx.state(), 2), -6.0f);
  SND_CHECK(a.ch(0)[kBlock - 1] > a.ch(0)[0]);  // drive ramps up
  // Out-of-range / NaN values are clamped / defaulted.
  float bad[4] = {5.0f, NAN, 100.0f, 1.0f};
  snd::ParamBlock pb2{bad, bad, 4, 0xFu, nullptr};
  Process(fx.state(), a, kBlock, &pb2);
  SND_CHECK_EQ(EffectParamValue(fx.state(), 0), 1.0f);
  SND_CHECK_EQ(EffectParamValue(fx.state(), 1), 1.0f);
  SND_CHECK_EQ(EffectParamValue(fx.state(), 2), 12.0f);
  SND_CHECK_EQ(EffectParamValue(fx.state(), 3), 500.0f);
  // Init-only parameters ignore the block.
  Instance echo(EffectKind::Echo, 1);
  float es[5] = {500, 0.5f, 1, 1, 3000.0f};
  snd::ParamBlock pe{es, es, 5, 0x1Fu, nullptr};
  Process(echo.state(), a, kBlock, &pe);
  SND_CHECK_EQ(EffectParamValue(echo.state(), 4), 1000.0f);
}

SND_TEST(Effects, CompressorStaticCurveAndSidechain) {
  // Hard knee, ratio 4, threshold -20 dB, fast detector: DC level in -> level out.
  const float ov[] = {-20.0f, 4.0f, 0.05f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  for (float inDb : {-40.0f, -25.0f, -20.0f, -10.0f, -4.0f, 0.0f}) {
    Instance fx(EffectKind::Compressor, 2, kBlock, ov, 8);
    Audio a(2, kBlock);
    for (int b = 0; b < 40; ++b) {
      for (std::uint32_t c = 0; c < 2; ++c)
        for (std::uint32_t i = 0; i < kBlock; ++i) a.ch(c)[i] = DbToGain(inDb);
      Process(fx.state(), a, kBlock);
    }
    const float outDb = GainToDb(a.ch(1)[kBlock - 1]);
    const float expect = inDb <= -20.0f ? inDb : -20.0f + (inDb + 20.0f) / 4.0f;
    SND_CHECK_MSG(std::fabs(outDb - expect) < 0.05f, "in %.1f out %.2f expect %.2f", static_cast<double>(inDb),
                  static_cast<double>(outDb), static_cast<double>(expect));
  }
  // Soft knee (6 dB): at the threshold the gain change is -(1 - 1/R) * K / 8.
  const float kn[] = {-20.0f, 4.0f, 0.05f, 1.0f, 6.0f, 0.0f, 0.0f, 1.0f};
  Instance soft(EffectKind::Compressor, 1, kBlock, kn, 8);
  Audio m(1, kBlock);
  for (int b = 0; b < 40; ++b) {
    for (std::uint32_t i = 0; i < kBlock; ++i) m.ch(0)[i] = DbToGain(-20.0f);
    Process(soft.state(), m, kBlock);
  }
  SND_CHECK_NEAR(GainToDb(m.ch(0)[kBlock - 1]), -20.0 - 0.75 * 6.0 / 8.0, 0.05);
  // Ducking: a quiet music signal is pulled down while the sidechain (voice) is loud.
  Instance duck(EffectKind::Compressor, 2, kBlock, ov, 8);
  Audio music(2, kBlock), voice(1, kBlock);
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    voice.ch(0)[i] = 0.9f;
    music.ch(0)[i] = music.ch(1)[i] = 0.05f;
  }
  EffectSidechain sc{voice.cp(), 1};
  for (int b = 0; b < 20; ++b) {
    for (std::uint32_t i = 0; i < kBlock; ++i) music.ch(0)[i] = music.ch(1)[i] = 0.05f;
    Process(duck.state(), music, kBlock, nullptr, &sc);
  }
  SND_CHECK(music.ch(0)[kBlock - 1] < 0.05f * DbToGain(-10.0f));
  voice.Zero();
  for (int b = 0; b < 100; ++b) {
    for (std::uint32_t i = 0; i < kBlock; ++i) music.ch(0)[i] = music.ch(1)[i] = 0.05f;
    Process(duck.state(), music, kBlock, nullptr, &sc);
  }
  SND_CHECK_NEAR(music.ch(0)[kBlock - 1], 0.05, 1e-3);
}

SND_TEST(Effects, LimiterHoldsTheCeiling) {
  const float ov[] = {-1.0f, 50.0f, 2.0f, 12.0f};
  Instance fx(EffectKind::Limiter, 2, kBlock, ov, 4);
  SND_REQUIRE(fx.ok);
  const std::uint32_t lat = EffectLatencyFrames(fx.state());
  SND_CHECK_EQ(lat, 96u - 1u);
  dsptest::Noise nz(5);
  Audio a(2, kBlock);
  const float ceil = DbToGain(-1.0f);
  float maxOut = 0.0f;
  for (int b = 0; b < 200; ++b) {
    for (std::uint32_t c = 0; c < 2; ++c)
      for (std::uint32_t i = 0; i < kBlock; ++i) a.ch(c)[i] = nz.Next(1.0f) * ((b / 10) % 2 ? 1.0f : 0.05f);
    Process(fx.state(), a, kBlock);
    maxOut = std::max(maxOut, std::max(Peak(a.ch(0), kBlock), Peak(a.ch(1), kBlock)));
  }
  SND_CHECK(maxOut <= ceil + 1e-6f);
  // Quiet material passes unchanged, delayed by the look-ahead.
  Instance q(EffectKind::Limiter, 1);
  std::vector<float> in, out;
  Audio m(1, kBlock);
  for (int b = 0; b < 4; ++b) {
    for (std::uint32_t i = 0; i < kBlock; ++i) m.ch(0)[i] = 0.1f * nz.Next();
    in.insert(in.end(), m.ch(0), m.ch(0) + kBlock);
    Process(q.state(), m, kBlock);
    out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
  }
  const std::uint32_t l2 = EffectLatencyFrames(q.state());
  SND_CHECK(dsptest::MaxAbsDiff(in.data(), out.data() + l2, 4 * kBlock - l2) < 1e-6f);
}

SND_TEST(Effects, BehaviourChecks) {
  Audio a(2, kBlock);
  // Echo: impulse returns after Delay ms scaled by WetMix, then Decay * WetMix.
  {
    const float ov[] = {10.0f, 0.5f, 1.0f, 0.8f, 100.0f};
    Instance fx(EffectKind::Echo, 1, kBlock, ov, 5);
    Audio m(1, kBlock);
    std::vector<float> out;
    for (int b = 0; b < 6; ++b) {
      m.Zero();
      if (b == 0) m.ch(0)[0] = 1.0f;
      Process(fx.state(), m, kBlock);
      out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
    }
    SND_CHECK_NEAR(out[0], 1.0, 1e-6);
    SND_CHECK_NEAR(out[480], 0.8, 1e-5);
    SND_CHECK_NEAR(out[960], 0.4, 1e-5);
  }
  // Parametric EQ: +6 dB peak at 1 kHz.
  {
    float ov[16];
    EffectDefaultParams(EffectKind::ParamEq, ov);
    ov[4] = 1.0f; ov[5] = 1000.0f; ov[6] = 1.0f; ov[7] = 6.0f;
    Instance fx(EffectKind::ParamEq, 1, kBlock, ov, 16);
    Audio m(1, kBlock);
    std::vector<float> out;
    for (int b = 0; b < 40; ++b) {
      for (std::uint32_t i = 0; i < kBlock; ++i)
        m.ch(0)[i] = 0.25f * static_cast<float>(std::sin(2 * dsptest::kPi * 1000.0 * (b * kBlock + i) / kRate));
      Process(fx.state(), m, kBlock);
      out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
    }
    SND_CHECK_NEAR(dsptest::Db(dsptest::ToneAmplitude(out.data() + 2048, 8192, 1000.0, kRate) / 0.25), 6.0, 0.05);
  }
  // Lowpass at 2 kHz: 8 kHz is down ~24 dB.
  {
    const float ov[] = {2000.0f, 0.70710678f};
    Instance fx(EffectKind::Lowpass, 1, kBlock, ov, 2);
    Audio m(1, kBlock);
    std::vector<float> out;
    for (int b = 0; b < 40; ++b) {
      for (std::uint32_t i = 0; i < kBlock; ++i)
        m.ch(0)[i] = 0.25f * static_cast<float>(std::sin(2 * dsptest::kPi * 8000.0 * (b * kBlock + i) / kRate));
      Process(fx.state(), m, kBlock);
      out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
    }
    const double expect = dsptest::Db(BiquadMagnitude(DesignBiquad(BiquadType::Lowpass, 2000.0f, 0.70710678f, 0.0f, 48000.0f), 8000.0, 48000.0));
    SND_CHECK(expect < -24.0);
    SND_CHECK_NEAR(dsptest::Db(dsptest::ToneAmplitude(out.data() + 2048, 8192, 8000.0, kRate) / 0.25), expect, 0.1);
  }
  // Pitch shifter: 440 Hz shifted by 2 -> energy at 880 Hz dominates 440 Hz.
  {
    const float ov[] = {2.0f, 50.0f, 1.0f};
    Instance fx(EffectKind::PitchShifter, 1, kBlock, ov, 3);
    Audio m(1, kBlock);
    std::vector<float> out;
    for (int b = 0; b < 60; ++b) {
      for (std::uint32_t i = 0; i < kBlock; ++i)
        m.ch(0)[i] = 0.5f * static_cast<float>(std::sin(2 * dsptest::kPi * 440.0 * (b * kBlock + i) / kRate));
      Process(fx.state(), m, kBlock);
      out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
    }
    const double a880 = dsptest::ToneAmplitude(out.data() + 4096, 8192, 880.0, kRate);
    const double a440 = dsptest::ToneAmplitude(out.data() + 4096, 8192, 440.0, kRate);
    SND_CHECK_MSG(a880 > 4.0 * a440, "880: %g 440: %g", a880, a440);
  }
  // Normalize: a quiet sine is raised to 1 / LowestVolume * peak.
  {
    const float ov[] = {100.0f, 0.1f, 20.0f};
    Instance fx(EffectKind::Normalize, 1, kBlock, ov, 3);
    Audio m(1, kBlock);
    for (int b = 0; b < 200; ++b) {
      for (std::uint32_t i = 0; i < kBlock; ++i)
        m.ch(0)[i] = 0.05f * static_cast<float>(std::sin(2 * dsptest::kPi * 500.0 * (b * kBlock + i) / kRate));
      Process(fx.state(), m, kBlock);
    }
    SND_CHECK_NEAR(Peak(m.ch(0), kBlock), 0.5, 0.01);
  }
  // Surround on 5.1: rears get signal, width 0 makes the fronts mono.
  {
    const float ov[] = {0.0f, -6.0f, 10.0f, 7000.0f, -60.0f};
    Instance fx(EffectKind::Surround, 6, kBlock, ov, 5);
    Audio m(6, kBlock);
    for (int b = 0; b < 4; ++b) {
      m.Zero();
      for (std::uint32_t i = 0; i < kBlock; ++i) {
        m.ch(0)[i] = 0.5f * static_cast<float>(std::sin(0.05 * (b * kBlock + i)));
        m.ch(1)[i] = 0.1f;
      }
      Process(fx.state(), m, kBlock);
    }
    SND_CHECK(Peak(m.ch(4), kBlock) > 0.05f);
    SND_CHECK(Peak(m.ch(5), kBlock) > 0.01f);
    SND_CHECK_EQ(Peak(m.ch(3), kBlock), 0.0f);
    SND_CHECK(dsptest::MaxAbsDiff(m.ch(0), m.ch(1), kBlock) < 1e-6f);
  }
  // Reverb: the decay follows DecayTime (energy over 50 ms windows, -60 dB slope).
  {
    float ov[14];
    EffectDefaultParams(EffectKind::Reverb, ov);
    ov[0] = -10000.0f;  // wet only
    ov[4] = 1.0f;       // 1 s
    ov[5] = 1.0f;       // no HF damping
    Instance fx(EffectKind::Reverb, 2, kBlock, ov, 14);
    Audio m(2, kBlock);
    std::vector<float> out;
    for (std::uint32_t b = 0; b < 48000u / kBlock; ++b) {
      m.Zero();
      if (b == 0) m.ch(0)[0] = m.ch(1)[0] = 1.0f;
      Process(fx.state(), m, kBlock);
      out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
    }
    const double e1 = dsptest::Rms(out.data() + 9600, 2400);    // 200..250 ms
    const double e2 = dsptest::Rms(out.data() + 33600, 2400);   // 700..750 ms
    const double slope = (dsptest::Db(e2) - dsptest::Db(e1)) / 0.5;  // dB per second
    snd::test::Note("reverb decay slope %.1f dB/s (target -60)", slope);
    SND_CHECK(slope < -45.0 && slope > -75.0);
    SND_CHECK(e1 > 1e-4);
  }
  (void)a;
}

namespace {
// A plugin effect: gain by params[0] (start/end ramp), state = one float.
class GainPlugin final : public snd::IDspEffect {
 public:
  std::uint32_t StateBytes(const snd::EffectDesc&) const override { return 4; }
  void Init(void* state, const snd::EffectDesc&, std::uint32_t) override { *static_cast<float*>(state) = 0.0f; }
  void Process(void* state, const float* const* in, float* const* out, std::uint32_t ch,
               std::uint32_t frames, const snd::ParamBlock& p) override {
    const float g = p.count > 0 ? p.end[0] : 1.0f;
    for (std::uint32_t c = 0; c < ch; ++c)
      for (std::uint32_t i = 0; i < frames; ++i) out[c][i] = in[c][i] * g;
    *static_cast<float*>(state) = g;
  }
  bool IsTailDone(const void*) const override { return true; }
};
}  // namespace

SND_TEST(Effects, ChainWithPluginAndBypass) {
  GainPlugin plugin;
  const snd::Id pluginId = snd::HashId("test.gain");
  const snd_dsp_effect table = snd::ToC(plugin, pluginId);
  snd::EffectDesc pd{};
  pd.effect_type = pluginId;
  pd.channels = 2;
  pd.max_frames = kBlock;
  dsptest::AlignedMem pstate(PluginStateBytes(table, pd), dsptest::kDirty);
  SND_CHECK_EQ(pstate.size(), 64u);
  table.init(table.user, pstate.get(), &pd, kRate);

  const float dv[] = {6.0f, 0.70710678f};  // highpass at 6 Hz ~ transparent for a 1 kHz tone
  Instance hp(EffectKind::Highpass, 2, kBlock, dv, 2);
  float gain = 0.5f;
  snd::ParamBlock pp{&gain, &gain, 1, 1u, nullptr};
  EffectSlot slots[2] = {{pluginId, &table, pstate.get(), &pp, nullptr, 0, 0},
                         {kEffectHighpass, nullptr, hp.state(), nullptr, nullptr, 0, 0}};
  Audio a(2, kBlock);
  for (std::uint32_t c = 0; c < 2; ++c)
    for (std::uint32_t i = 0; i < kBlock; ++i) a.ch(c)[i] = 1.0f;
  {
    SND_RT_SCOPE();
    ProcessEffectChain(slots, 2, a.p(), 2, kBlock);
  }
  SND_CHECK_NEAR(a.ch(1)[0], 0.5, 1e-3);
  SND_CHECK_EQ(*static_cast<float*>(pstate.get()), 0.5f);
  slots[0].bypass = 1;
  slots[1].bypass = 1;
  for (std::uint32_t i = 0; i < kBlock; ++i) a.ch(0)[i] = 1.0f;
  ProcessEffectChain(slots, 2, a.p(), 2, kBlock);
  SND_CHECK_EQ(a.ch(0)[100], 1.0f);
  SND_CHECK(EffectSlotIsTailDone(slots[0]));
  // Batch over two instances of the same kind.
  Instance e1(EffectKind::Echo, 2), e2(EffectKind::Echo, 2);
  Audio b1(2, kBlock), b2(2, kBlock);
  b1.ch(0)[0] = 1.0f;
  EffectBatchItem items[2] = {{e1.state(), b1.cp(), b1.p(), 2, kBlock, nullptr, nullptr},
                              {e2.state(), b2.cp(), b2.p(), 2, kBlock, nullptr, nullptr}};
  {
    SND_RT_SCOPE();
    EffectProcessBatch(EffectKind::Echo, items, 2);
  }
  SND_CHECK(!EffectIsTailDone(e1.state()));
  SND_CHECK(EffectIsTailDone(e2.state()));
  EffectReset(e1.state());
  SND_CHECK(EffectIsTailDone(e1.state()));
}

SND_TEST(Effects, PoolSizing) {
  GainPlugin plugin;
  const snd::Id pluginId = snd::HashId("test.gain");
  const snd_dsp_effect table = snd::ToC(plugin, pluginId);
  float rp[14];
  EffectDefaultParams(EffectKind::Reverb, rp);
  snd::EffectDesc descs[4] = {};
  descs[0] = {kEffectReverb, 2, 256, 14, rp, nullptr, 0};
  descs[1] = {kEffectEcho, 2, 256, 0, nullptr, nullptr, 0};
  descs[2] = {pluginId, 2, 256, 0, nullptr, nullptr, 0};
  descs[3] = {snd::HashId("unknown.fx"), 2, 256, 0, nullptr, nullptr, 0};
  std::uint32_t unknown = 0;
  const auto need = ComputeDspPoolNeed(descs, 4, kRate, &table, 1, &unknown);
  SND_CHECK_EQ(unknown, 1u);
  SND_CHECK_EQ(need.instanceCount, 3u);
  SND_CHECK_EQ(need.instanceBytes, static_cast<std::uint64_t>(EffectStateBytes(descs[0], kRate)) +
                                       EffectStateBytes(descs[1], kRate) + 64u);
  SND_CHECK_EQ(need.instanceBytes % 64u, 0u);
  SND_CHECK_EQ(EffectInstanceBytes(descs[2], kRate, nullptr, 0), 0u);
  // An echo with a longer MaxDelay needs more memory.
  float ep[5] = {500, 0.5f, 1, 1, 4000};
  snd::EffectDesc longEcho{kEffectEcho, 2, 256, 5, ep, nullptr, 0};
  SND_CHECK(EffectStateBytes(longEcho, kRate) > EffectStateBytes(descs[1], kRate));
  // Sized for 48 kHz, run at 96 kHz: init succeeds with clamped capacity.
  dsptest::AlignedMem mem(EffectStateBytes(longEcho, kRate), dsptest::kDirty);
  SND_CHECK(EffectInit(mem.get(), mem.size(), longEcho, 96000));
  Audio a(2, kBlock);
  a.ch(0)[0] = 1.0f;
  Process(mem.get(), a, kBlock);
  SND_CHECK(!HasNonFinite(a.ch(0), kBlock));
  SND_CHECK(!EffectInit(mem.get(), 64, longEcho, kRate));  // far too small
}

// Decision b: one NaN sample -> that block's non-finite output is zeroed, the
// state is reset at the end of the block, and the next blocks are normal.
SND_TEST(Effects, RecoversFromNonFiniteInput) {
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    Instance fx(kind, 2), fresh(kind, 2);
    SND_REQUIRE(fx.ok && fresh.ok);
    Audio a(2, kBlock), f(2, kBlock);
    TestSignal(a, 0, kBlock);
    a.ch(0)[17] = NAN;
    a.ch(1)[3] = INFINITY;
    Process(fx.state(), a, kBlock);
    SND_CHECK_MSG(!HasNonFinite(a.ch(0), kBlock) && !HasNonFinite(a.ch(1), kBlock), "%s: non-finite output",
                  ShortName(kind));
    // After the reset the effect behaves exactly like a fresh instance.
    float diff = 0.0f;
    for (std::uint32_t b = 1; b < 8; ++b) {
      TestSignal(a, b * kBlock, kBlock);
      TestSignal(f, b * kBlock, kBlock);
      Process(fx.state(), a, kBlock);
      Process(fresh.state(), f, kBlock);
      for (std::uint32_t c = 0; c < 2; ++c) diff = std::max(diff, dsptest::MaxAbsDiff(a.ch(c), f.ch(c), kBlock));
    }
    SND_CHECK_MSG(diff == 0.0f, "%s: differs from a fresh instance after recovery by %g", ShortName(kind),
                  static_cast<double>(diff));
    // Empty calls do nothing.
    Process(fx.state(), a, 0);
    SND_RT_SCOPE();
    EffectProcess(fx.state(), a.cp(), a.p(), 0, kBlock, nullptr, nullptr);
  }
}

// Decision c: EffectReset == a fresh EffectInit (LFO phases included).
SND_TEST(Effects, ResetEqualsFreshInit) {
  for (EffectKind kind : {EffectKind::Chorus, EffectKind::Flanger, EffectKind::PitchShifter, EffectKind::Reverb}) {
    Instance used(kind, 2), fresh(kind, 2);
    Audio a(2, kBlock), f(2, kBlock);
    for (std::uint32_t b = 0; b < 30; ++b) {
      TestSignal(a, b * kBlock, kBlock);
      Process(used.state(), a, kBlock);
    }
    {
      SND_RT_SCOPE();
      EffectReset(used.state());
    }
    float diff = 0.0f;
    for (std::uint32_t b = 0; b < 8; ++b) {
      TestSignal(a, b * kBlock, kBlock);
      TestSignal(f, b * kBlock, kBlock);
      Process(used.state(), a, kBlock);
      Process(fresh.state(), f, kBlock);
      for (std::uint32_t c = 0; c < 2; ++c) diff = std::max(diff, dsptest::MaxAbsDiff(a.ch(c), f.ch(c), kBlock));
    }
    SND_CHECK_MSG(diff == 0.0f, "%s: reset differs from fresh by %g", ShortName(kind), static_cast<double>(diff));
  }
}

// Item 4: the pitch shifter at Pitch = 1 is the input delayed by its reported latency.
SND_TEST(Effects, PitchShifterUnityIsDelayedInput) {
  for (float mix : {1.0f, 0.5f, 0.0f}) {
    const float ov[] = {1.0f, 20.0f, mix};
    Instance fx(EffectKind::PitchShifter, 1, kBlock, ov, 3);
    const std::uint32_t lat = EffectLatencyFrames(fx.state());
    SND_CHECK_EQ(lat, 480u);
    std::vector<float> in, out;
    Audio m(1, kBlock);
    dsptest::Noise nz(4);
    for (int b = 0; b < 8; ++b) {
      for (std::uint32_t i = 0; i < kBlock; ++i) m.ch(0)[i] = nz.Next(0.5f);
      in.insert(in.end(), m.ch(0), m.ch(0) + kBlock);
      Process(fx.state(), m, kBlock);
      out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
    }
    SND_CHECK(dsptest::MaxAbsDiff(in.data(), out.data() + lat, 8 * kBlock - lat) < 1e-6f);
  }
}

// Ramped parameters split at the matching intermediate values (documented
// tolerance in effects.hpp): every parameter ramps from its default to 70 % of
// the way to its maximum over 1024 frames, processed as one call vs 4 x 256.
SND_TEST(Effects, RampedBlockSplitDifference) {
  constexpr std::uint32_t kN = 1024;
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    const EffectInfo& info = GetEffectInfo(kind);
    float s[kMaxEffectParams], e[kMaxEffectParams];
    EffectDefaultParams(kind, s);
    for (std::uint32_t p = 0; p < info.paramCount; ++p) {
      const bool integer = (info.params[p].flags & kParamInteger) != 0;
      e[p] = integer ? s[p] : s[p] + 0.7f * (info.params[p].maxValue - s[p]);
    }
    Instance a(kind, 2, kN), b(kind, 2, kN);
    Audio xa(2, kN), xb(2, kN);
    TestSignal(xa, 0, kN);
    TestSignal(xb, 0, kN);
    snd::ParamBlock full{s, e, info.paramCount, 0xFFFFFFFFu, nullptr};
    Process(a.state(), xa, kN, &full);
    for (std::uint32_t q = 0; q < 4; ++q) {
      float ms[kMaxEffectParams], me[kMaxEffectParams];
      for (std::uint32_t p = 0; p < info.paramCount; ++p) {
        ms[p] = s[p] + (e[p] - s[p]) * (static_cast<float>(q) / 4.0f);
        me[p] = s[p] + (e[p] - s[p]) * (static_cast<float>(q + 1) / 4.0f);
      }
      snd::ParamBlock part{ms, me, info.paramCount, 0xFFFFFFFFu, nullptr};
      float* p2[2] = {xb.ch(0) + q * 256, xb.ch(1) + q * 256};
      EffectProcess(b.state(), p2, p2, 2, 256, &part, nullptr);
    }
    const float d = std::max(dsptest::MaxAbsDiff(xa.ch(0), xb.ch(0), kN), dsptest::MaxAbsDiff(xa.ch(1), xb.ch(1), kN));
    snd::test::Note("%-11s ramped split vs single call: max diff %.3g", ShortName(kind), static_cast<double>(d));
    SND_CHECK_MSG(d < 2e-4f, "%s ramped split diff %g (documented < 2e-4)", ShortName(kind), static_cast<double>(d));
  }
}

// Every effect with a non-zero EffectLatencyFrames: at rates where the
// latency in frames is fractional, the output for transparent settings is the
// input delayed by exactly the reported latency (bit-exact).
SND_TEST(Effects, ReportedLatencyIsExactAtAllRates) {
  for (std::uint32_t rate : {22050u, 44100u, 48000u, 88200u, 96000u, 192000u}) {
    struct Case {
      EffectKind kind;
      float ov[4];
      std::uint32_t count;
      const char* what;
    };
    const Case cases[] = {
        {EffectKind::PitchShifter, {1.0f, 50.0f, 1.0f, 0.0f}, 3, "pitch 1 mix 1"},
        {EffectKind::PitchShifter, {1.0f, 33.3f, 0.37f, 0.0f}, 3, "pitch 1 mix 0.37"},
        {EffectKind::PitchShifter, {1.0f, 10.0f, 0.0f, 0.0f}, 3, "pitch 1 mix 0"},
        {EffectKind::Limiter, {0.0f, 50.0f, 2.0f, 0.0f}, 4, "quiet, 2 ms"},
        {EffectKind::Limiter, {0.0f, 50.0f, 0.73f, 0.0f}, 4, "quiet, 0.73 ms"},
    };
    for (const Case& cs : cases) {
      float params[kMaxEffectParams];
      EffectDefaultParams(cs.kind, params);
      for (std::uint32_t i = 0; i < cs.count; ++i) params[i] = cs.ov[i];
      snd::EffectDesc d{GetEffectInfo(cs.kind).typeId, 1, kBlock, GetEffectInfo(cs.kind).paramCount, params, nullptr, 0};
      dsptest::AlignedMem mem(EffectStateBytes(d, rate), dsptest::kDirty);
      SND_REQUIRE(EffectInit(mem.get(), mem.size(), d, rate));
      const std::uint32_t lat = EffectLatencyFrames(mem.get());
      std::vector<float> in, out;
      Audio m(1, kBlock);
      dsptest::Noise nz(rate);
      const std::uint32_t blocks = (lat / kBlock) + 4;
      for (std::uint32_t b = 0; b < blocks; ++b) {
        for (std::uint32_t i = 0; i < kBlock; ++i) m.ch(0)[i] = nz.Next(0.25f);
        in.insert(in.end(), m.ch(0), m.ch(0) + kBlock);
        Process(mem.get(), m, kBlock);
        out.insert(out.end(), m.ch(0), m.ch(0) + kBlock);
      }
      const float diff = dsptest::MaxAbsDiff(in.data(), out.data() + lat, blocks * kBlock - lat);
      SND_CHECK_MSG(diff == 0.0f, "%s %s at %u Hz: latency %u, differs from the delayed input by %g",
                    ShortName(cs.kind), cs.what, rate, lat, static_cast<double>(diff));
    }
  }
}
