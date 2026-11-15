// Basic kernels, panning and downmix.
#include <cmath>

#include "dsp_test_util.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/dsp/pan.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/simd.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

SND_TEST(Kernels, PathReported) {
  snd::test::Note("SIMD path in dsp tests: %s", snd::simd::kPathName);
}

SND_TEST(Kernels, GainRampsAndMix) {
  constexpr std::uint32_t kN = 37;  // not a multiple of the vector width
  Audio src(1, kN), dst(1, kN);
  for (std::uint32_t i = 0; i < kN; ++i) src.ch(0)[i] = 1.0f + 0.01f * static_cast<float>(i);
  {
    SND_RT_SCOPE();
    CopyGainRamp(dst.ch(0), src.ch(0), kN, 0.5f, 1.5f);
  }
  for (std::uint32_t i = 0; i < kN; ++i) {
    const float g = 0.5f + (1.0f / kN) * static_cast<float>(i);
    SND_CHECK_NEAR(dst.ch(0)[i], src.ch(0)[i] * g, 1e-6);
  }
  {
    SND_RT_SCOPE();
    MixGainRamp(dst.ch(0), src.ch(0), kN, 2.0f, 2.0f);
  }
  for (std::uint32_t i = 0; i < kN; ++i) {
    const float g = 0.5f + (1.0f / kN) * static_cast<float>(i);
    SND_CHECK_NEAR(dst.ch(0)[i], src.ch(0)[i] * (g + 2.0f), 1e-5);
  }
  // Block split continuity: the ramp of [0, n) equals the ramps of [0, k) and [k, n).
  Audio a(1, 256), b(1, 256), s(1, 256);
  for (std::uint32_t i = 0; i < 256; ++i) s.ch(0)[i] = 1.0f;
  CopyGainRamp(a.ch(0), s.ch(0), 256, 0.0f, 1.0f);
  CopyGainRamp(b.ch(0), s.ch(0), 100, 0.0f, 100.0f / 256.0f);
  CopyGainRamp(b.ch(0) + 100, s.ch(0) + 100, 156, 100.0f / 256.0f, 1.0f);
  SND_CHECK(dsptest::MaxAbsDiff(a.ch(0), b.ch(0), 256) < 1e-6f);
  Scale(a.ch(0), 256, 0.0f);
  SND_CHECK_EQ(Peak(a.ch(0), 256), 0.0f);
}

SND_TEST(Kernels, PeakSumInterleave) {
  Audio a(2, 101);
  for (std::uint32_t i = 0; i < 101; ++i) {
    a.ch(0)[i] = std::sin(0.1f * static_cast<float>(i));
    a.ch(1)[i] = -0.5f * static_cast<float>(i) / 100.0f;
  }
  a.ch(0)[50] = NAN;
  Audio back(2, 101);
  Audio three(3, 10);
  float inter[202];
  float inter3[30];
  SND_RT_SCOPE();
  SND_CHECK_NEAR(Peak(a.ch(1), 101), 0.5f, 1e-7);
  SND_CHECK(Peak(a.ch(0), 101) <= 1.0f);
  SND_CHECK(HasNonFinite(a.ch(0), 101));
  SND_CHECK(!HasNonFinite(a.ch(1), 101));
  double ref = 0.0;
  for (std::uint32_t i = 0; i < 101; ++i) ref += static_cast<double>(a.ch(1)[i]) * a.ch(1)[i];
  SND_CHECK_NEAR(SumOfSquares(a.ch(1), 101), ref, 1e-5);
  Interleave(a.cp(), 2, 101, inter);
  SND_CHECK_EQ(inter[3], a.ch(1)[1]);
  Deinterleave(inter, 2, 101, back.p());
  SND_CHECK_EQ(back.ch(1)[77], a.ch(1)[77]);
  for (std::uint32_t c = 0; c < 3; ++c)
    for (std::uint32_t i = 0; i < 10; ++i) three.ch(c)[i] = static_cast<float>(c * 100 + i);
  Interleave(three.cp(), 3, 10, inter3);
  SND_CHECK_EQ(inter3[3 * 4 + 2], 204.0f);
  SND_CHECK(!HasDenormal(inter3, 30));
  const float tiny = 1e-40f;
  SND_CHECK(HasDenormal(&tiny, 1));
  SND_CHECK_NEAR(DbToGain(-6.0206f), 0.5f, 1e-4);
  SND_CHECK_NEAR(GainToDb(0.1f), -20.0f, 1e-4);
  SND_CHECK_EQ(GainToDb(0.0f), kMinDb);
}

SND_TEST(Pan, StereoLawsPowerAndLevels) {
  for (int k = -10; k <= 10; ++k) {
    const float pan = static_cast<float>(k) / 10.0f;
    float l, r;
    StereoPanGains(pan, PanLaw::ConstantPower, &l, &r);
    SND_CHECK_NEAR(l * l + r * r, 1.0, 1e-6);
    StereoPanGains(pan, PanLaw::Linear, &l, &r);
    SND_CHECK_NEAR(l + r, 1.0, 1e-6);
  }
  float l, r;
  StereoPanGains(0.0f, PanLaw::ConstantPower, &l, &r);
  SND_CHECK_NEAR(dsptest::Db(l), -3.0103, 1e-3);
  StereoPanGains(0.0f, PanLaw::Compromise, &l, &r);
  SND_CHECK_NEAR(dsptest::Db(l), -4.515, 1e-2);
  StereoPanGains(1.0f, PanLaw::ConstantPower, &l, &r);
  SND_CHECK_NEAR(l, 0.0, 1e-6);
  SND_CHECK_NEAR(r, 1.0, 1e-6);
}

SND_TEST(Pan, PowerPreservedForEveryLayoutAzimuthSpread) {
  const SpeakerLayout layouts[] = {SpeakerLayout::Mono, SpeakerLayout::Stereo, SpeakerLayout::Quad,
                                   SpeakerLayout::Surround51, SpeakerLayout::Surround71};
  for (SpeakerLayout layout : layouts) {
    const std::uint32_t n = LayoutChannels(layout);
    for (int a = -36; a <= 36; ++a) {
      for (float spread : {0.0f, 0.1f, 0.5f, 1.0f}) {
        float g[8] = {};
        ComputePanGains(layout, static_cast<float>(a) * 5.0f * 3.14159265f / 180.0f, spread, g);
        double p = 0.0;
        for (std::uint32_t c = 0; c < n; ++c) {
          p += static_cast<double>(g[c]) * g[c];
          SND_CHECK_MSG(g[c] >= 0.0f, "layout %u az %d spread %.1f ch %u g %g", static_cast<unsigned>(layout), a, static_cast<double>(spread), c, static_cast<double>(g[c]));
        }
        SND_CHECK_NEAR(p, 1.0, 1e-5);
        const int lfe = LayoutLfeChannel(layout);
        if (lfe >= 0) SND_CHECK_EQ(g[lfe], 0.0f);
      }
    }
  }
  // A source on a speaker uses only that speaker.
  float g[8];
  ComputePanGains(SpeakerLayout::Surround51, 110.0f * 3.14159265f / 180.0f, 0.0f, g);
  SND_CHECK_NEAR(g[5], 1.0, 1e-5);
  SND_CHECK_NEAR(g[0] + g[1] + g[2] + g[4], 0.0, 1e-5);
  ComputePanGains(SpeakerLayout::Surround71, 0.0f, 0.0f, g);
  SND_CHECK_NEAR(g[2], 1.0, 1e-5);
  // Behind in 5.1: between SL and SR.
  ComputePanGains(SpeakerLayout::Surround51, 3.14159265f, 0.0f, g);
  SND_CHECK_NEAR(g[4], g[5], 1e-4);
  // Full spread distributes over every speaker.
  ComputePanGains(SpeakerLayout::Quad, 0.0f, 1.0f, g);
  for (int c = 0; c < 4; ++c) SND_CHECK_NEAR(g[c], 0.5, 0.02);
}

SND_TEST(Pan, DownmixGains) {
  snd::PanMatrix m;
  ComputeDownmixMatrix(SpeakerLayout::Surround51, SpeakerLayout::Stereo, &m);
  SND_CHECK_NEAR(m.gain[0][0], 1.0, 1e-6);
  SND_CHECK_NEAR(m.gain[2][0], 0.70710678, 1e-6);
  SND_CHECK_NEAR(m.gain[2][1], 0.70710678, 1e-6);
  SND_CHECK_EQ(m.gain[3][0], 0.0f);  // LFE dropped
  SND_CHECK_NEAR(m.gain[4][0], 0.70710678, 1e-6);
  SND_CHECK_EQ(m.gain[4][1], 0.0f);
  ComputeDownmixMatrix(SpeakerLayout::Surround51, SpeakerLayout::Stereo, &m, true);
  SND_CHECK_NEAR(m.gain[3][1], 0.70710678, 1e-6);
  // 7.1 -> stereo goes through 5.1: back/side surrounds at 0.5 into their side.
  ComputeDownmixMatrix(SpeakerLayout::Surround71, SpeakerLayout::Stereo, &m);
  SND_CHECK_NEAR(m.gain[4][0], 0.5, 1e-6);
  SND_CHECK_NEAR(m.gain[6][0], 0.5, 1e-6);
  SND_CHECK_NEAR(m.gain[7][1], 0.5, 1e-6);
  SND_CHECK_NEAR(m.gain[2][1], 0.70710678, 1e-6);
  ComputeDownmixMatrix(SpeakerLayout::Stereo, SpeakerLayout::Mono, &m);
  SND_CHECK_NEAR(m.gain[0][0] * m.gain[0][0] + m.gain[1][0] * m.gain[1][0], 1.0, 1e-6);
  ComputeDownmixMatrix(SpeakerLayout::Mono, SpeakerLayout::Surround51, &m);
  SND_CHECK_EQ(m.gain[0][2], 1.0f);
  ComputeDownmixMatrix(SpeakerLayout::Quad, SpeakerLayout::Quad, &m);
  for (int c = 0; c < 4; ++c) SND_CHECK_EQ(m.gain[c][c], 1.0f);
  // Uncorrelated equal-power 5.1 surround content keeps its power in stereo.
  ComputeDownmixMatrix(SpeakerLayout::Surround71, SpeakerLayout::Surround51, &m);
  SND_CHECK_NEAR(m.gain[4][4] * m.gain[4][4] + m.gain[6][4] * m.gain[6][4], 1.0, 1e-6);

  // 2D pan of a stereo source: pan = 0 is the plain downmix, pan = 1 removes the left.
  ComputeStereoPanMatrix(2, SpeakerLayout::Stereo, 0.0f, PanLaw::ConstantPower, &m);
  SND_CHECK_EQ(m.gain[0][0], 1.0f);
  SND_CHECK_EQ(m.gain[1][1], 1.0f);
  ComputeStereoPanMatrix(2, SpeakerLayout::Stereo, 1.0f, PanLaw::ConstantPower, &m);
  SND_CHECK_NEAR(m.gain[0][0], 0.0, 1e-6);
  SND_CHECK_EQ(m.gain[1][1], 1.0f);
  ComputeStereoPanMatrix(1, SpeakerLayout::Surround51, 0.0f, PanLaw::ConstantPower, &m);
  SND_CHECK_NEAR(m.gain[0][0] * m.gain[0][0] + m.gain[0][1] * m.gain[0][1], 1.0, 1e-6);
}

SND_TEST(Pan, MatrixApplyRampsAndMultiChannelSources) {
  constexpr std::uint32_t kN = 64;
  Audio in(2, kN), out(2, kN);
  for (std::uint32_t i = 0; i < kN; ++i) {
    in.ch(0)[i] = 1.0f;
    in.ch(1)[i] = 2.0f;
  }
  snd::PanMatrix a, b;
  ClearPanMatrix(&a);
  ClearPanMatrix(&b);
  a.gain[0][0] = 1.0f;
  b.gain[0][0] = 0.0f;
  b.gain[1][1] = 1.0f;
  {
    SND_RT_SCOPE();
    ApplyPanMatrix(in.cp(), 2, out.p(), 2, kN, a, b, false);
  }
  for (std::uint32_t i = 0; i < kN; ++i) {
    const float t = static_cast<float>(i) / kN;
    SND_CHECK_NEAR(out.ch(0)[i], 1.0f - t, 1e-6);
    SND_CHECK_NEAR(out.ch(1)[i], 2.0f * t, 1e-6);
  }
  {
    SND_RT_SCOPE();
    ApplyPanMatrix(in.cp(), 2, out.p(), 2, kN, b, b, true);
  }
  SND_CHECK_NEAR(out.ch(1)[kN - 1], 2.0f * (kN - 1) / kN + 2.0f, 1e-5);
  // 3D: a stereo source at spread 0 collapses to one point with rows scaled by 1/sqrt(2).
  snd::PanMatrix m;
  ComputePanMatrix(2, SpeakerLayout::Stereo, 0.0f, 0.0f, &m);
  SND_CHECK_NEAR(m.gain[0][0], 0.5, 1e-5);
  SND_CHECK_NEAR(m.gain[1][1], 0.5, 1e-5);
  ComputePanMatrix(2, SpeakerLayout::Surround51, 0.0f, 1.0f, &m);  // +-45 deg
  SND_CHECK(m.gain[0][0] > m.gain[0][1]);
  SND_CHECK(m.gain[1][1] > m.gain[1][0]);
  snd::PanMatrix l;
  LerpPanMatrix(a, b, 0.5f, &l);
  SND_CHECK_EQ(l.gain[0][0], 0.5f);
  ScalePanMatrix(&l, 2.0f);
  SND_CHECK_EQ(l.gain[1][1], 1.0f);
}
