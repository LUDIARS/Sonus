// Independent pan / downmix / meter tests (pan.hpp, meter.hpp), checked
// against the numbers written in those headers and against EBU Tech 3341.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/dsp/meter.hpp"
#include "snd/dsp/pan.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

namespace {

constexpr double kDegD = 3.14159265358979323846 / 180.0;
const SpeakerLayout kLayouts[] = {SpeakerLayout::Mono, SpeakerLayout::Stereo, SpeakerLayout::Quad,
                                  SpeakerLayout::Surround51, SpeakerLayout::Surround71};
const char* LName(SpeakerLayout l) {
  static const char* n[] = {"mono", "stereo", "quad", "5.1", "7.1"};
  return n[static_cast<int>(l)];
}
float Rad(double deg) { return static_cast<float>(deg * kDegD); }

// Speaker azimuths as written in pan.hpp ("L/R +-30 deg, C 0, SL/SR +-110 deg
// (5.1) or +-90 deg (7.1), BL/BR +-135 deg (quad) / +-150 deg (7.1)"), WAVE order.
struct Spk {
  std::uint32_t ch;
  double az;
};
const Spk kQuad[] = {{0, -30}, {1, 30}, {2, -135}, {3, 135}};
const Spk k51[] = {{0, -30}, {1, 30}, {2, 0}, {4, -110}, {5, 110}};
const Spk k71[] = {{0, -30}, {1, 30}, {2, 0}, {4, -150}, {5, 150}, {6, -90}, {7, 90}};

// Mirror channel (left <-> right) in WAVE order.
std::uint32_t Mirror(SpeakerLayout l, std::uint32_t c) {
  if (l == SpeakerLayout::Mono) return c;
  if (c == 2 && (l == SpeakerLayout::Surround51 || l == SpeakerLayout::Surround71)) return c;  // C
  if (c == 3 && (l == SpeakerLayout::Surround51 || l == SpeakerLayout::Surround71)) return c;  // LFE
  return c ^ 1u;
}

}  // namespace

// Fine azimuth grid (0.25 deg) x spreads: power exactly preserved, gains >= 0,
// LFE 0, left/right mirror symmetry, and no jumps between neighbouring angles.
SND_TEST(PanIndep, PowerSymmetryContinuityFineGrid) {
  for (SpeakerLayout l : kLayouts) {
    const std::uint32_t n = LayoutChannels(l);
    double worstPow = 0.0, worstSym = 0.0, worstJump = 0.0;
    for (float spread : {0.0f, 0.02f, 0.25f, 0.5f, 0.77f, 1.0f}) {
      float prev[8] = {};
      for (int k = -720; k <= 720; ++k) {
        const double az = k * 0.25;
        float g[8] = {}, m[8] = {};
        ComputePanGains(l, Rad(az), spread, g);
        ComputePanGains(l, Rad(-az), spread, m);
        double p = 0.0;
        for (std::uint32_t c = 0; c < n; ++c) {
          p += static_cast<double>(g[c]) * g[c];
          SND_CHECK(g[c] >= 0.0f && std::isfinite(g[c]));
          worstSym = std::max(worstSym, static_cast<double>(std::fabs(g[c] - m[Mirror(l, c)])));
          if (k > -720) worstJump = std::max(worstJump, static_cast<double>(std::fabs(g[c] - prev[c])));
        }
        std::copy(g, g + 8, prev);
        worstPow = std::max(worstPow, std::fabs(p - 1.0));
        const int lfe = LayoutLfeChannel(l);
        if (lfe >= 0) SND_CHECK_EQ(g[lfe], 0.0f);
      }
    }
    snd::test::Note("%-6s power error %.2g, mirror asymmetry %.2g, max gain jump per 0.25 deg %.3g", LName(l), worstPow,
                    worstSym, worstJump);
    SND_CHECK(worstPow < 1e-5);
    SND_CHECK(worstSym < 1e-4);
    SND_CHECK(worstJump < 0.05);
  }
  // NaN azimuth / spread do not produce NaN.
  float g[8];
  ComputePanGains(SpeakerLayout::Surround71, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(), g);
  for (float v : g) SND_CHECK(std::isfinite(v));
  ComputePanGains(SpeakerLayout::Surround51, 1e30f, 2.0f, g);
  for (int c = 0; c < 6; ++c) SND_CHECK(std::isfinite(g[c]));
}

// A point source exactly on a speaker plays from that speaker only, at the
// azimuths pan.hpp documents (stereo uses pan = sin(azimuth): +-90 deg).
SND_TEST(PanIndep, SourceOnSpeakerUsesOnlyThatSpeaker_51And71) {
  struct L {
    SpeakerLayout l;
    const Spk* s;
    std::uint32_t n;
  };
  const L ls[] = {{SpeakerLayout::Surround51, k51, 5}, {SpeakerLayout::Surround71, k71, 7}};
  for (const L& x : ls) {
    for (std::uint32_t i = 0; i < x.n; ++i) {
      float g[8] = {};
      ComputePanGains(x.l, Rad(x.s[i].az), 0.0f, g);
      for (std::uint32_t c = 0; c < LayoutChannels(x.l); ++c) {
        const float expect = c == x.s[i].ch ? 1.0f : 0.0f;
        SND_CHECK_MSG(std::fabs(g[c] - expect) < 1e-5f, "%s source at %.0f deg: ch %u gain %.5f (expected %.0f)", LName(x.l),
                      x.s[i].az, c, static_cast<double>(g[c]), static_cast<double>(expect));
      }
    }
  }
  float g[8];
  ComputePanGains(SpeakerLayout::Stereo, Rad(90), 0.0f, g);
  SND_CHECK_NEAR(g[1], 1.0, 1e-6);
  SND_CHECK_NEAR(g[0], 0.0, 1e-6);
  ComputePanGains(SpeakerLayout::Stereo, Rad(-90), 0.0f, g);
  SND_CHECK_NEAR(g[0], 1.0, 1e-6);
  // Rear folds to the front (documented): 150 deg == 30 deg.
  float h[8];
  ComputePanGains(SpeakerLayout::Stereo, Rad(150), 0.0f, g);
  ComputePanGains(SpeakerLayout::Stereo, Rad(30), 0.0f, h);
  SND_CHECK_NEAR(g[0], h[0], 1e-6);
  SND_CHECK_NEAR(g[1], h[1], 1e-6);
}

// pan.hpp: "Speaker azimuths: L/R +-30 deg ... BL/BR +-135 deg (quad)". The
// quad layout places L/R at +-45 deg instead, so a source at +-30 deg is split
// between L and R.
SND_TEST(PanIndep, QuadFrontSpeakersAtDocumentedAzimuth) {
  for (const Spk& s : kQuad) {
    float g[8] = {};
    ComputePanGains(SpeakerLayout::Quad, Rad(s.az), 0.0f, g);
    for (std::uint32_t c = 0; c < 4; ++c) {
      const float expect = c == s.ch ? 1.0f : 0.0f;
      SND_CHECK_MSG(std::fabs(g[c] - expect) < 1e-5f, "quad source at %.0f deg: ch %u gain %.5f (expected %.0f)", s.az, c,
                    static_cast<double>(g[c]), static_cast<double>(expect));
    }
  }
}

// Multi-channel sources: row structure, total power 1, LFE output column 0.
SND_TEST(PanIndep, PanMatrixStructure) {
  for (SpeakerLayout out : kLayouts) {
    for (std::uint32_t in = 1; in <= 8; ++in) {
      for (float spread : {0.0f, 0.4f, 1.0f}) {
        for (double az : {0.0, 33.0, -100.0, 180.0}) {
          snd::PanMatrix m;
          ComputePanMatrix(in, out, Rad(az), spread, &m);
          double p = 0.0;
          for (std::uint32_t i = 0; i < 8; ++i)
            for (std::uint32_t o = 0; o < 8; ++o) {
              if (i >= in || o >= LayoutChannels(out)) SND_CHECK_EQ(m.gain[i][o], 0.0f);
              p += static_cast<double>(m.gain[i][o]) * m.gain[i][o];
            }
          SND_CHECK_MSG(std::fabs(p - 1.0) < 1e-5, "%u ch -> %s spread %.1f az %.0f: total power %.6f", in, LName(out),
                        static_cast<double>(spread), az, p);
          const int lfe = LayoutLfeChannel(out);
          if (lfe >= 0)
            for (std::uint32_t i = 0; i < in; ++i) SND_CHECK_EQ(m.gain[i][lfe], 0.0f);
          if (spread == 0.0f && in > 1) {
            for (std::uint32_t i = 1; i < in; ++i)
              for (std::uint32_t o = 0; o < 8; ++o) SND_CHECK_NEAR(m.gain[i][o], m.gain[0][o], 1e-6);
          }
        }
      }
    }
  }
  snd::PanMatrix z;
  ComputePanMatrix(0, SpeakerLayout::Stereo, 0.0f, 0.0f, &z);
  for (auto& row : z.gain)
    for (float v : row) SND_CHECK_EQ(v, 0.0f);
}

// Every downmix coefficient written in pan.hpp.
SND_TEST(PanIndep, DownmixCoefficientsAsDocumented) {
  using L = SpeakerLayout;
  const float h = 0.70710678f;
  auto expect = [](const snd::PanMatrix& m, std::uint32_t inN, std::uint32_t outN,
                   std::initializer_list<std::tuple<int, int, float>> nz, const char* what) {
    float e[8][8] = {};
    for (const auto& [i, o, v] : nz) e[i][o] = v;
    for (std::uint32_t i = 0; i < 8; ++i)
      for (std::uint32_t o = 0; o < 8; ++o) {
        const float want = (i < inN && o < outN) ? e[i][o] : 0.0f;
        SND_CHECK_MSG(std::fabs(m.gain[i][o] - want) < 1e-5f, "%s: gain[%u][%u] = %.5f, documented %.5f", what, i, o,
                      static_cast<double>(m.gain[i][o]), static_cast<double>(want));
      }
  };
  snd::PanMatrix m;
  ComputeDownmixMatrix(L::Surround51, L::Stereo, &m);
  expect(m, 6, 2, {{0, 0, 1}, {1, 1, 1}, {2, 0, h}, {2, 1, h}, {4, 0, h}, {5, 1, h}}, "5.1->stereo");
  ComputeDownmixMatrix(L::Surround51, L::Stereo, &m, true);
  expect(m, 6, 2, {{0, 0, 1}, {1, 1, 1}, {2, 0, h}, {2, 1, h}, {3, 0, h}, {3, 1, h}, {4, 0, h}, {5, 1, h}}, "5.1->stereo +LFE");
  // (LFE -> LFE between layouts that both have one: pan.hpp's "LFE is dropped
  // unless includeLfe" does not say; the implementation keeps it. Spec gap.)
  ComputeDownmixMatrix(L::Surround71, L::Surround51, &m);
  expect(m, 8, 6, {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}, {3, 3, m.gain[3][3]}, {4, 4, h}, {6, 4, h}, {5, 5, h}, {7, 5, h}}, "7.1->5.1");
  snd::test::Note("7.1->5.1 without includeLfe: LFE->LFE gain %.3f", static_cast<double>(m.gain[3][3]));
  ComputeDownmixMatrix(L::Surround71, L::Surround51, &m, true);
  expect(m, 8, 6, {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}, {3, 3, 1}, {4, 4, h}, {6, 4, h}, {5, 5, h}, {7, 5, h}}, "7.1->5.1 +LFE");
  ComputeDownmixMatrix(L::Quad, L::Stereo, &m);
  expect(m, 4, 2, {{0, 0, 1}, {1, 1, 1}, {2, 0, h}, {3, 1, h}}, "quad->stereo");
  ComputeDownmixMatrix(L::Stereo, L::Mono, &m);
  expect(m, 2, 1, {{0, 0, h}, {1, 0, h}}, "stereo->mono");
  ComputeDownmixMatrix(L::Quad, L::Mono, &m);
  expect(m, 4, 1, {{0, 0, h}, {1, 0, h}, {2, 0, 0.5f}, {3, 0, 0.5f}}, "quad->mono");
  ComputeDownmixMatrix(L::Surround51, L::Mono, &m);
  expect(m, 6, 1, {{0, 0, h}, {1, 0, h}, {2, 0, 1}, {4, 0, 0.5f}, {5, 0, 0.5f}}, "5.1->mono");
  ComputeDownmixMatrix(L::Mono, L::Stereo, &m);
  expect(m, 1, 2, {{0, 0, h}, {0, 1, h}}, "mono->stereo");
  ComputeDownmixMatrix(L::Mono, L::Surround51, &m);
  expect(m, 1, 6, {{0, 2, 1}}, "mono->5.1");
  ComputeDownmixMatrix(L::Mono, L::Surround71, &m);
  expect(m, 1, 8, {{0, 2, 1}}, "mono->7.1");
  ComputeDownmixMatrix(L::Stereo, L::Surround51, &m);
  expect(m, 2, 6, {{0, 0, 1}, {1, 1, 1}}, "stereo->5.1");
  ComputeDownmixMatrix(L::Stereo, L::Surround71, &m);
  expect(m, 2, 8, {{0, 0, 1}, {1, 1, 1}}, "stereo->7.1");
  for (L l : kLayouts) {
    ComputeDownmixMatrix(l, l, &m);
    for (std::uint32_t i = 0; i < 8; ++i)
      for (std::uint32_t o = 0; o < 8; ++o)
        SND_CHECK_EQ(m.gain[i][o], (i == o && i < LayoutChannels(l)) ? 1.0f : 0.0f);
  }
  // 7.1 -> stereo must equal 7.1 -> 5.1 -> stereo.
  snd::PanMatrix a, b, c;
  ComputeDownmixMatrix(L::Surround71, L::Surround51, &a);
  ComputeDownmixMatrix(L::Surround51, L::Stereo, &b);
  MultiplyPanMatrix(a, b, &c);
  ComputeDownmixMatrix(L::Surround71, L::Stereo, &m);
  for (std::uint32_t i = 0; i < 8; ++i)
    for (std::uint32_t o = 0; o < 8; ++o) SND_CHECK_NEAR(m.gain[i][o], c.gain[i][o], 1e-6);
}

// pan.hpp: "any -> mono: 0.7071 L + 0.7071 R + C + 0.5 surrounds". For 7.1 the
// four surrounds (BL BR SL SR) get 0.3536 (= 0.5 * 0.7071) instead.
SND_TEST(PanIndep, Downmix71ToMonoSurroundsAtHalf) {
  snd::PanMatrix m;
  ComputeDownmixMatrix(SpeakerLayout::Surround71, SpeakerLayout::Mono, &m);
  SND_CHECK_NEAR(m.gain[0][0], 0.70710678, 1e-5);
  SND_CHECK_NEAR(m.gain[1][0], 0.70710678, 1e-5);
  SND_CHECK_NEAR(m.gain[2][0], 1.0, 1e-5);
  SND_CHECK_EQ(m.gain[3][0], 0.0f);
  for (std::uint32_t c = 4; c < 8; ++c)
    SND_CHECK_MSG(std::fabs(m.gain[c][0] - 0.5f) < 1e-5f, "7.1->mono surround ch %u gain %.4f, documented 0.5", c,
                  static_cast<double>(m.gain[c][0]));
}

// Stereo pan laws and the 2D balance matrix.
SND_TEST(PanIndep, StereoLawsAndBalance) {
  for (int k = -100; k <= 100; ++k) {
    const float p = static_cast<float>(k) / 100.0f;
    float l, r, l2, r2;
    StereoPanGains(p, PanLaw::ConstantPower, &l, &r);
    SND_CHECK_NEAR(l * l + r * r, 1.0, 2e-6);
    StereoPanGains(-p, PanLaw::ConstantPower, &l2, &r2);
    SND_CHECK_NEAR(l, r2, 1e-6);
    StereoPanGains(p, PanLaw::Linear, &l, &r);
    SND_CHECK_NEAR(l, 0.5 * (1.0 - p), 1e-6);
    StereoPanGains(p, PanLaw::Compromise, &l, &r);
    float cl, cr, ll, lr;
    StereoPanGains(p, PanLaw::ConstantPower, &cl, &cr);
    StereoPanGains(p, PanLaw::Linear, &ll, &lr);
    SND_CHECK_NEAR(l, std::sqrt(cl * ll), 1e-6);
  }
  float l, r;
  StereoPanGains(std::numeric_limits<float>::quiet_NaN(), PanLaw::Linear, &l, &r);
  SND_CHECK_EQ(l, 0.5f);
  StereoPanGains(-7.0f, PanLaw::ConstantPower, &l, &r);
  SND_CHECK_NEAR(l, 1.0, 1e-6);
  SND_CHECK_EQ(r, 0.0f);
  // Balance: pan 0 = plain downmix; full right silences the left column only.
  snd::PanMatrix m, d;
  ComputeStereoPanMatrix(6, SpeakerLayout::Stereo, 0.0f, PanLaw::ConstantPower, &m);
  ComputeDownmixMatrix(SpeakerLayout::Surround51, SpeakerLayout::Stereo, &d);
  for (std::uint32_t i = 0; i < 8; ++i)
    for (std::uint32_t o = 0; o < 8; ++o) SND_CHECK_EQ(m.gain[i][o], d.gain[i][o]);
  ComputeStereoPanMatrix(2, SpeakerLayout::Surround51, 1.0f, PanLaw::Linear, &m);
  SND_CHECK_EQ(m.gain[0][0], 0.0f);
  SND_CHECK_EQ(m.gain[1][1], 1.0f);
  ComputeStereoPanMatrix(1, SpeakerLayout::Stereo, -0.5f, PanLaw::ConstantPower, &m);
  StereoPanGains(-0.5f, PanLaw::ConstantPower, &l, &r);
  SND_CHECK_EQ(m.gain[0][0], l);
  SND_CHECK_EQ(m.gain[0][1], r);
}

// ApplyPanMatrix: D-26 ramp from `from` (frame 0) towards `to`, consecutive
// blocks continue without a step; accumulate / overwrite; untouched columns.
SND_TEST(PanIndep, ApplyPanMatrixRampsAndModes) {
  constexpr std::uint32_t kN = 50;
  for (std::uint32_t in = 1; in <= 8; ++in) {
    for (std::uint32_t outN = 1; outN <= 8; ++outN) {
      snd::PanMatrix a, b;
      ClearPanMatrix(&a);
      ClearPanMatrix(&b);
      for (std::uint32_t i = 0; i < in; ++i)
        for (std::uint32_t o = 0; o < outN; ++o) {
          a.gain[i][o] = ((i + o) % 3 == 0) ? 0.0f : 0.1f * static_cast<float>(i + 1);
          b.gain[i][o] = ((i * o) % 4 == 1) ? 0.0f : -0.05f * static_cast<float>(o + 1);
        }
      Audio x(in, kN), y(outN, kN);
      dsptest::Noise nz(in * 9 + outN);
      for (std::uint32_t c = 0; c < in; ++c)
        for (std::uint32_t i = 0; i < kN; ++i) x.ch(c)[i] = nz.Next();
      for (std::uint32_t c = 0; c < outN; ++c)
        for (std::uint32_t i = 0; i < kN; ++i) y.ch(c)[i] = 99.0f;
      {
        SND_RT_SCOPE();
        ApplyPanMatrix(x.cp(), in, y.p(), outN, kN, a, b, false);
      }
      double worst = 0.0;
      for (std::uint32_t o = 0; o < outN; ++o)
        for (std::uint32_t i = 0; i < kN; ++i) {
          double e = 0.0;
          for (std::uint32_t c = 0; c < in; ++c)
            e += x.ch(c)[i] * (a.gain[c][o] + (static_cast<double>(b.gain[c][o]) - a.gain[c][o]) * i / kN);
          worst = std::max(worst, std::fabs(y.ch(o)[i] - e));
        }
      SND_CHECK_MSG(worst < 1e-5, "%u -> %u: overwrite error %g", in, outN, worst);
      // Accumulate adds on top.
      std::vector<float> keep(y.ch(0), y.ch(0) + kN);
      ApplyPanMatrix(x.cp(), in, y.p(), outN, kN, b, b, true);
      for (std::uint32_t i = 0; i < kN; ++i) {
        double e = keep[i];
        for (std::uint32_t c = 0; c < in; ++c) e += static_cast<double>(x.ch(c)[i]) * b.gain[c][0];
        SND_CHECK_NEAR(y.ch(0)[i], e, 1e-5);
      }
    }
  }
  // All-zero matrix with accumulate = false clears the outputs.
  snd::PanMatrix z;
  ClearPanMatrix(&z);
  Audio x(2, 8), y(2, 8);
  for (std::uint32_t i = 0; i < 8; ++i) y.ch(0)[i] = y.ch(1)[i] = 5.0f;
  ApplyPanMatrix(x.cp(), 2, y.p(), 2, 8, z, z, false);
  SND_CHECK_EQ(Peak(y.ch(0), 8) + Peak(y.ch(1), 8), 0.0f);
  // Lerp / Scale / Multiply.
  snd::PanMatrix p, q, r;
  ComputePanMatrix(2, SpeakerLayout::Surround71, 0.3f, 0.2f, &p);
  ComputePanMatrix(2, SpeakerLayout::Surround71, -2.0f, 0.9f, &q);
  LerpPanMatrix(p, q, 0.0f, &r);
  for (std::uint32_t i = 0; i < 8; ++i)
    for (std::uint32_t o = 0; o < 8; ++o) SND_CHECK_EQ(r.gain[i][o], p.gain[i][o]);
  LerpPanMatrix(p, q, 1.0f, &r);
  for (std::uint32_t i = 0; i < 8; ++i)
    for (std::uint32_t o = 0; o < 8; ++o) SND_CHECK_NEAR(r.gain[i][o], q.gain[i][o], 1e-7);
  snd::PanMatrix id;
  ComputeDownmixMatrix(SpeakerLayout::Surround71, SpeakerLayout::Surround71, &id);
  MultiplyPanMatrix(p, id, &r);
  for (std::uint32_t i = 0; i < 8; ++i)
    for (std::uint32_t o = 0; o < 8; ++o) SND_CHECK_EQ(r.gain[i][o], p.gain[i][o]);
  MultiplyPanMatrix(p, id, &p);  // aliasing out == a
  ScalePanMatrix(&r, 2.0f);
  SND_CHECK_EQ(r.gain[0][0], 2.0f * p.gain[0][0]);
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------
namespace {

struct Seg {
  double seconds;
  double dbfs;  // per-channel sine level (peak), -inf = silence
};

// 1 kHz sine segments into `chans` channels with per-channel extra gain (dB).
std::unique_ptr<LoudnessMeterState> RunLoudness(std::uint32_t chans, std::uint32_t rate, std::initializer_list<Seg> segs,
                                                const double* chanDb, std::uint32_t block, double* minShort = nullptr,
                                                double* maxShort = nullptr) {
  auto lm = std::make_unique<LoudnessMeterState>();
  LoudnessMeterInit(*lm, chans, rate);
  Audio a(chans, block);
  std::uint64_t t = 0;
  if (minShort) *minShort = 1e9;
  if (maxShort) *maxShort = -1e9;
  for (const Seg& s : segs) {
    const std::uint64_t n = static_cast<std::uint64_t>(s.seconds * rate + 0.5);
    for (std::uint64_t done = 0; done < n;) {
      const std::uint32_t k = static_cast<std::uint32_t>(std::min<std::uint64_t>(block, n - done));
      for (std::uint32_t c = 0; c < chans; ++c) {
        const double g = std::pow(10.0, (s.dbfs + chanDb[c]) / 20.0);
        for (std::uint32_t i = 0; i < k; ++i)
          a.ch(c)[i] = static_cast<float>(g * std::sin(2 * dsptest::kPi * 1000.0 * static_cast<double>(t + i) / rate));
      }
      {
        SND_RT_SCOPE();
        LoudnessMeterProcess(*lm, a.cp(), k);
      }
      t += k;
      done += k;
      if (minShort && t > 3u * rate + rate / 2) {
        *minShort = std::min(*minShort, static_cast<double>(LoudnessShortTerm(*lm)));
        *maxShort = std::max(*maxShort, static_cast<double>(LoudnessShortTerm(*lm)));
      }
    }
  }
  return lm;
}

}  // namespace

// EBU Tech 3341 (v4) minimum requirements, tolerance +-0.1 LU.
SND_TEST(MeterIndep, EbuTech3341Cases) {
  const double st[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (std::uint32_t rate : {48000u, 44100u}) {
    // Case 1: stereo -23 dBFS, 20 s -> M, S, I = -23.0.
    auto m = RunLoudness(2, rate, {{20, -23}}, st, 480);
    SND_CHECK_NEAR(LoudnessMomentary(*m), -23.0, 0.1);
    SND_CHECK_NEAR(LoudnessShortTerm(*m), -23.0, 0.1);
    SND_CHECK_NEAR(LoudnessIntegrated(*m), -23.0, 0.1);
    // Case 2: -33 dBFS -> -33.0.
    m = RunLoudness(2, rate, {{20, -33}}, st, 480);
    SND_CHECK_NEAR(LoudnessMomentary(*m), -33.0, 0.1);
    SND_CHECK_NEAR(LoudnessShortTerm(*m), -33.0, 0.1);
    SND_CHECK_NEAR(LoudnessIntegrated(*m), -33.0, 0.1);
    // Case 3: -36 / -23 / -36 dBFS for 10 / 60 / 10 s -> I = -23.0 (relative gate).
    m = RunLoudness(2, rate, {{10, -36}, {60, -23}, {10, -36}}, st, 1000);
    SND_CHECK_NEAR(LoudnessIntegrated(*m), -23.0, 0.1);
    // Case 4: -72 / -36 / -23 / -36 / -72 dBFS for 10 / 10 / 60 / 10 / 10 s -> -23.0.
    m = RunLoudness(2, rate, {{10, -72}, {10, -36}, {60, -23}, {10, -36}, {10, -72}}, st, 1000);
    SND_CHECK_NEAR(LoudnessIntegrated(*m), -23.0, 0.1);
    // Case 5: -26 / -20 / -26 dBFS for 20 / 20.1 / 20 s -> -23.0.
    m = RunLoudness(2, rate, {{20, -26}, {20.1, -20}, {20, -26}}, st, 1000);
    SND_CHECK_NEAR(LoudnessIntegrated(*m), -23.0, 0.1);
    // Case 6: 5.0 (L R C Ls Rs at -28 -28 -24 -30 -30 dBFS) in a 5.1 stream (LFE silent) -> -23.0.
    const double ch6[6] = {-28, -28, -24, -300, -30, -30};
    m = RunLoudness(6, rate, {{20, 0}}, ch6, 480);
    SND_CHECK_NEAR(LoudnessIntegrated(*m), -23.0, 0.1);
    // Case 9: 20 x (1.34 s at -20 dBFS + 1.66 s at -30 dBFS): S = -23.0 at all times after 3 s.
    double lo = 0, hi = 0;
    m = RunLoudness(2, rate,
                    {{1.34, -20}, {1.66, -30}, {1.34, -20}, {1.66, -30}, {1.34, -20}, {1.66, -30}, {1.34, -20}, {1.66, -30},
                     {1.34, -20}, {1.66, -30}, {1.34, -20}, {1.66, -30}, {1.34, -20}, {1.66, -30}, {1.34, -20}, {1.66, -30}},
                    st, 480, &lo, &hi);
    snd::test::Note("%u Hz case 9: short-term range [%.3f, %.3f] LUFS", rate, lo, hi);
    SND_CHECK(lo > -23.1 && hi < -22.9);
  }
}

// 7.1 surround weights (+1.5 dB on BL BR SL SR) and LFE exclusion; quad rears.
SND_TEST(MeterIndep, ChannelWeights) {
  for (std::uint32_t chans : {4u, 6u, 8u}) {
    for (std::uint32_t c = 0; c < chans; ++c) {
      double g[8] = {-300, -300, -300, -300, -300, -300, -300, -300};
      g[c] = -20.0;
      auto m = RunLoudness(chans, 48000, {{6, 0}}, g, 480);
      const bool lfe = chans >= 6 && c == 3;
      const bool surround = (chans == 4 && c >= 2) || (chans >= 6 && c >= 4);
      // One channel at -20 dBFS sine: -20 - 3.01 dB (mono vs stereo) [+1.49 dB surround].
      const double expect = lfe ? kLoudnessSilence : -20.0 - 3.0103 + (surround ? 10.0 * std::log10(1.41) : 0.0);
      SND_CHECK_MSG(std::fabs(LoudnessIntegrated(*m) - expect) < 0.1, "%u ch, channel %u: %.3f LUFS, expected %.3f", chans, c,
                    static_cast<double>(LoudnessIntegrated(*m)), expect);
    }
  }
}

// K-weighting at 48 kHz equals the BS.1770-4 coefficient table.
SND_TEST(MeterIndep, KWeightingMatchesBs1770Table) {
  auto lm = std::make_unique<LoudnessMeterState>();
  LoudnessMeterInit(*lm, 2, 48000);
  const double shelfB[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
  const double shelfA[2] = {-1.69065929318241, 0.73248077421585};
  const double hpA[2] = {-1.99004745483398, 0.99007225036621};
  for (int i = 0; i < 3; ++i) SND_CHECK_NEAR(lm->shelf[i], shelfB[i], 2e-6);
  SND_CHECK_NEAR(lm->shelf[3], shelfA[0], 2e-6);
  SND_CHECK_NEAR(lm->shelf[4], shelfA[1], 2e-6);
  SND_CHECK_NEAR(lm->highpass[0], 1.0, 1e-9);
  SND_CHECK_NEAR(lm->highpass[1], -2.0, 1e-9);
  SND_CHECK_NEAR(lm->highpass[2], 1.0, 1e-9);
  SND_CHECK_NEAR(lm->highpass[3], hpA[0], 2e-6);
  SND_CHECK_NEAR(lm->highpass[4], hpA[1], 2e-6);
}

// Block split invariance (1 frame per call vs 4800), absolute gate, silence, reset.
SND_TEST(MeterIndep, LoudnessSplitGateSilenceReset) {
  const double st[2] = {0, 0};
  auto a = RunLoudness(2, 48000, {{4, -18}, {2, -40}}, st, 4800);
  auto b = RunLoudness(2, 48000, {{4, -18}, {2, -40}}, st, 1);
  SND_CHECK_NEAR(LoudnessIntegrated(*a), LoudnessIntegrated(*b), 1e-4);
  SND_CHECK_NEAR(LoudnessShortTerm(*a), LoudnessShortTerm(*b), 1e-4);
  SND_CHECK_NEAR(LoudnessMomentary(*a), LoudnessMomentary(*b), 1e-4);
  // Below the absolute gate (-70 LUFS): integrated stays "silence".
  auto q = RunLoudness(2, 48000, {{5, -75}}, st, 480);
  SND_CHECK_EQ(LoudnessIntegrated(*q), kLoudnessSilence);
  SND_CHECK_NEAR(LoudnessMomentary(*q), -75.0, 0.2);
  auto z = RunLoudness(2, 48000, {{2, -1000}}, st, 480);
  SND_CHECK_EQ(LoudnessIntegrated(*z), kLoudnessSilence);
  SND_CHECK_EQ(LoudnessMomentary(*z), kLoudnessSilence);
  SND_CHECK_EQ(LoudnessShortTerm(*z), kLoudnessSilence);
  LoudnessMeterReset(*a);
  SND_CHECK_EQ(LoudnessIntegrated(*a), kLoudnessSilence);
  SND_CHECK_EQ(a->channels, 2u);
  SND_CHECK_EQ(a->sampleRate, 48000u);
}

SND_TEST(MeterIndep, PeakAndRmsKnownSignals) {
  // Peak hold / release: 1.0, then 1 s of silence at 20 dB/s -> 0.1.
  PeakMeterState pk;
  PeakMeterInit(pk, 2, 20.0f, 48000);
  Audio a(2, 480);
  a.ch(1)[17] = -1.0f;
  a.ch(0)[3] = 0.25f;
  PeakMeterProcess(pk, a.cp(), 480);
  SND_CHECK_EQ(pk.peak[1], 1.0f);
  SND_CHECK_EQ(pk.peak[0], 0.25f);
  a.Zero();
  for (int b = 0; b < 100; ++b) PeakMeterProcess(pk, a.cp(), 480);
  SND_CHECK_NEAR(pk.peak[1], 0.1, 0.002);
  PeakMeterInit(pk, 1, 0.0f, 48000);  // hold forever
  a.ch(0)[0] = 0.5f;
  PeakMeterProcess(pk, a.cp(), 480);
  a.Zero();
  for (int b = 0; b < 100; ++b) PeakMeterProcess(pk, a.cp(), 480);
  SND_CHECK_EQ(pk.peak[0], 0.5f);
  // RMS of a sine = A / sqrt(2); of DC = DC.
  RmsMeterState rm;
  RmsMeterInit(rm, 2, 0.05f, 48000);
  Audio s(2, 480);
  for (int b = 0; b < 200; ++b) {
    for (std::uint32_t i = 0; i < 480; ++i) {
      s.ch(0)[i] = 0.8f * static_cast<float>(std::sin(2 * dsptest::kPi * 1000.0 * (b * 480 + i) / 48000.0));
      s.ch(1)[i] = -0.3f;
    }
    RmsMeterProcess(rm, s.cp(), 480);
  }
  SND_CHECK_NEAR(RmsMeterValue(rm, 0), 0.8 / std::sqrt(2.0), 1e-3);
  SND_CHECK_NEAR(RmsMeterValue(rm, 1), 0.3, 1e-4);
  SND_CHECK_EQ(RmsMeterValue(rm, 5), 0.0f);
}

// FFT against a naive DFT; spectrum normalisation (bin-centred sine reads its
// amplitude, Hann neighbours read half), DC, Nyquist, silence, sizes.
SND_TEST(MeterIndep, FftAndSpectrum) {
  for (std::uint32_t n : {32u, 64u, 256u, 1024u}) {
    std::vector<float> tw(n), data(2 * n);
    FftTwiddles(tw.data(), n);
    dsptest::Noise nz(n);
    std::vector<std::complex<double>> x(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      data[2 * i] = nz.Next();
      data[2 * i + 1] = nz.Next();
      x[i] = {data[2 * i], data[2 * i + 1]};
    }
    FftComplex(data.data(), tw.data(), n);
    double worst = 0.0;
    for (std::uint32_t k = 0; k < n; ++k) {
      std::complex<double> s = 0;
      for (std::uint32_t i = 0; i < n; ++i) s += x[i] * std::polar(1.0, -2 * dsptest::kPi * k * i / n);
      worst = std::max(worst, std::abs(s - std::complex<double>(data[2 * k], data[2 * k + 1])));
    }
    SND_CHECK_MSG(worst < 1e-4 * std::sqrt(static_cast<double>(n)), "FFT %u: max error %g", n, worst);
  }
  for (std::uint32_t n : {32u, 512u, 8192u}) {
    dsptest::AlignedMem mem(SpectrumStateBytes(n));
    SND_REQUIRE(SpectrumInit(mem.get(), n));
    std::vector<float> mag(n / 2 + 1);
    const std::uint32_t bin = n / 8 + 1;
    Audio a(3, n);
    for (std::uint32_t i = 0; i < n; ++i) {
      const float v = 0.6f * static_cast<float>(std::sin(2 * dsptest::kPi * bin * i / n));
      a.ch(0)[i] = v;
      a.ch(1)[i] = v;
      a.ch(2)[i] = v;
    }
    {
      SND_RT_SCOPE();
      SpectrumPush(mem.get(), a.cp(), 3, n);
      SpectrumCompute(mem.get(), mag.data());
    }
    SND_CHECK_NEAR(mag[bin], 0.6, 2e-3);
    SND_CHECK_NEAR(mag[bin - 1], 0.3, 2e-3);
    SND_CHECK_NEAR(mag[bin + 1], 0.3, 2e-3);
    float far = 0.0f;
    for (std::uint32_t k = 0; k <= n / 2; ++k)
      if (k + 2 < bin || k > bin + 2) far = std::max(far, mag[k]);
    SND_CHECK_MSG(far < 1e-4f, "n %u: leakage %g", n, static_cast<double>(far));
    // DC of 0.5 reads 0.5 at bin 0; Nyquist (+-0.5 alternating) reads 0.5 at n/2.
    for (std::uint32_t i = 0; i < n; ++i) a.ch(0)[i] = 0.5f;
    SpectrumPush(mem.get(), a.cp(), 1, n);
    SpectrumCompute(mem.get(), mag.data());
    SND_CHECK_NEAR(mag[0], 0.5, 1e-3);
    for (std::uint32_t i = 0; i < n; ++i) a.ch(0)[i] = (i & 1) ? -0.5f : 0.5f;
    SpectrumPush(mem.get(), a.cp(), 1, n);
    SpectrumCompute(mem.get(), mag.data());
    SND_CHECK_NEAR(mag[n / 2], 0.5, 1e-3);
    a.Zero();
    SpectrumPush(mem.get(), a.cp(), 1, n);
    SpectrumCompute(mem.get(), mag.data());
    for (float v : mag) SND_CHECK_EQ(v, 0.0f);
  }
  for (std::uint32_t bad : {0u, 16u, 31u, 33u, 1000u, 16384u}) {
    SND_CHECK_EQ(SpectrumStateBytes(bad), 0u);
    dsptest::AlignedMem mem(64 * 1024);
    SND_CHECK(!SpectrumInit(mem.get(), bad));
  }
}

// RT safety of the per-block helpers not exercised under SND_RT_SCOPE elsewhere.
SND_TEST(MeterIndep, RemainingHelpersAreRtSafe) {
  auto lm = std::make_unique<LoudnessMeterState>();
  LoudnessMeterInit(*lm, 6, 48000);
  PeakMeterState pk;
  PeakMeterInit(pk, 6, 20.0f, 48000);
  RmsMeterState rm;
  RmsMeterInit(rm, 6, 0.3f, 48000);
  dsptest::AlignedMem rs(ResamplerStateBytes(ResamplerQuality::Sinc, 6));
  Audio a(6, 256);
  for (std::uint32_t c = 0; c < 6; ++c)
    for (std::uint32_t i = 0; i < 256; ++i) a.ch(c)[i] = 0.1f * static_cast<float>(c + 1);
  snd::PanMatrix m, n, o;
  float g[8];
  snd::FilterState fs;
  BiquadState bs;
  SND_RT_SCOPE();
  ComputePanGains(SpeakerLayout::Surround71, 0.7f, 0.3f, g);
  ComputePanMatrix(4, SpeakerLayout::Surround51, -0.2f, 0.5f, &m);
  ComputeStereoPanMatrix(6, SpeakerLayout::Stereo, 0.3f, PanLaw::Compromise, &n);
  ComputeDownmixMatrix(SpeakerLayout::Surround71, SpeakerLayout::Stereo, &o, true);
  LerpPanMatrix(m, n, 0.4f, &o);
  MultiplyPanMatrix(m, n, &o);
  ScalePanMatrix(&o, 0.5f);
  PeakMeterProcess(pk, a.cp(), 256);
  RmsMeterProcess(rm, a.cp(), 256);
  LoudnessMeterProcess(*lm, a.cp(), 256);
  (void)LoudnessMomentary(*lm);
  (void)LoudnessShortTerm(*lm);
  (void)LoudnessIntegrated(*lm);
  (void)RmsMeterValue(rm, 0);
  ResamplerInit(rs.get(), ResamplerQuality::Sinc, 6);
  ResamplerReset(rs.get());
  (void)ResamplerPendingOffset(rs.get());
  VoiceFilterInit(fs, snd::FilterTarget{1000.0f, 50.0f});
  (void)VoiceFilterIsOpen(fs, 48000);
  VoiceFilterProcessRamp(fs, a.p(), 6, 256, snd::FilterTarget{1000.0f, 50.0f}, snd::FilterTarget{3000.0f, 20.0f}, 48000);
  BiquadReset(bs);
  const BiquadCoefs c = DesignBiquad(BiquadType::HighShelf, 4000.0f, 0.0f, 6.0f, 48000.0f);
  BiquadProcess(bs, kBiquadIdentity, c, a.cp(), a.p(), 6, 256);
  BiquadFlush(bs, 6);
  (void)OctaveBandwidthToQ(1.5f);
  (void)DbToGain(-6.0f);
  (void)GainToDb(0.5f);
}
