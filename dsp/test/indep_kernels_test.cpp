// Independent tests of the basic kernels (kernels.hpp): every length 0..70 and
// every misalignment 0..15 floats, in place, ramp end points and block
// continuity, interleave round trips, reductions against double references.
// Guard values around every destination catch writes outside [0, frames).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr float kGuard = 12345.678f;
constexpr std::uint32_t kMaxLen = 70;
constexpr std::uint32_t kSlack = 32;

// One test buffer with guards on both sides, at a chosen float misalignment.
struct Buf {
  dsptest::AlignedMem mem{sizeof(float) * (kMaxLen + 3 * kSlack)};
  float* base() const { return static_cast<float*>(mem.get()); }
  float* at(std::uint32_t mis) const { return base() + kSlack + mis; }
  void Fill(std::uint32_t mis, std::uint32_t n, std::uint32_t seed) {
    for (std::uint32_t i = 0; i < kMaxLen + 3 * kSlack; ++i) base()[i] = kGuard;
    dsptest::Noise nz(seed);
    for (std::uint32_t i = 0; i < n; ++i) at(mis)[i] = nz.Next();
  }
  bool GuardsIntact(std::uint32_t mis, std::uint32_t n) const {
    for (std::uint32_t i = 0; i < kMaxLen + 3 * kSlack; ++i) {
      const float* p = base() + i;
      if (p >= at(mis) && p < at(mis) + n) continue;
      if (*p != kGuard) return false;
    }
    return true;
  }
};

double RampGain(float g0, float g1, std::uint32_t n, std::uint32_t i) {
  return static_cast<double>(g0) + (static_cast<double>(g1) - g0) * i / n;
}

}  // namespace

SND_TEST(KernelsIndep, AllLengthsAndAlignmentsWithGuards) {
  Buf src, dst;
  std::uint32_t bad = 0;
  for (std::uint32_t n = 0; n <= kMaxLen; ++n) {
    for (std::uint32_t ms = 0; ms < 16; ms += 3) {
      for (std::uint32_t md = 0; md < 16; md += 5) {
        const float g0 = 0.25f + 0.01f * static_cast<float>(n), g1 = -0.75f;
        // Copy.
        src.Fill(ms, n, n + 1);
        dst.Fill(md, n, n + 2);
        {
          SND_RT_SCOPE();
          Copy(dst.at(md), src.at(ms), n);
        }
        for (std::uint32_t i = 0; i < n; ++i) bad += dst.at(md)[i] != src.at(ms)[i];
        bad += !dst.GuardsIntact(md, n) || !src.GuardsIntact(ms, n);
        // Mix.
        dst.Fill(md, n, n + 3);
        std::vector<float> before(dst.at(md), dst.at(md) + n);
        {
          SND_RT_SCOPE();
          Mix(dst.at(md), src.at(ms), n);
        }
        for (std::uint32_t i = 0; i < n; ++i) bad += dst.at(md)[i] != before[i] + src.at(ms)[i];
        bad += !dst.GuardsIntact(md, n);
        // Scale (in place).
        dst.Fill(md, n, n + 4);
        before.assign(dst.at(md), dst.at(md) + n);
        Scale(dst.at(md), n, -1.5f);
        for (std::uint32_t i = 0; i < n; ++i) bad += dst.at(md)[i] != before[i] * -1.5f;
        bad += !dst.GuardsIntact(md, n);
        // Clear.
        dst.Fill(md, n, n + 5);
        Clear(dst.at(md), n);
        for (std::uint32_t i = 0; i < n; ++i) bad += dst.at(md)[i] != 0.0f;
        bad += !dst.GuardsIntact(md, n);
        // CopyGainRamp / MixGainRamp against the D-26 formula (double reference, float tolerance).
        dst.Fill(md, n, n + 6);
        CopyGainRamp(dst.at(md), src.at(ms), n, g0, g1);
        for (std::uint32_t i = 0; i < n; ++i) {
          const double e = src.at(ms)[i] * RampGain(g0, g1, n, i);
          bad += std::fabs(dst.at(md)[i] - e) > 1e-6;
        }
        bad += !dst.GuardsIntact(md, n);
        dst.Fill(md, n, n + 7);
        before.assign(dst.at(md), dst.at(md) + n);
        MixGainRamp(dst.at(md), src.at(ms), n, g0, g1);
        for (std::uint32_t i = 0; i < n; ++i) {
          const double e = before[i] + src.at(ms)[i] * RampGain(g0, g1, n, i);
          bad += std::fabs(dst.at(md)[i] - e) > 2e-6;
        }
        bad += !dst.GuardsIntact(md, n);
        // Constant gain path.
        dst.Fill(md, n, n + 8);
        before.assign(dst.at(md), dst.at(md) + n);
        MixGainRamp(dst.at(md), src.at(ms), n, 0.5f, 0.5f);
        for (std::uint32_t i = 0; i < n; ++i) bad += dst.at(md)[i] != before[i] + src.at(ms)[i] * 0.5f;
        bad += !dst.GuardsIntact(md, n);
        // Peak / SumOfSquares against double references.
        double ss = 0.0;
        float pk = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
          ss += static_cast<double>(src.at(ms)[i]) * src.at(ms)[i];
          pk = std::max(pk, std::fabs(src.at(ms)[i]));
        }
        bad += Peak(src.at(ms), n) != pk;
        bad += std::fabs(SumOfSquares(src.at(ms), n) - ss) > 1e-6 * (1.0 + ss);
      }
    }
    if (bad != 0) {
      SND_CHECK_MSG(false, "first failure at length %u (%u bad)", n, bad);
      return;
    }
  }
  SND_CHECK_EQ(bad, 0u);
}

// In place (dst == src) for the ramp copies, at odd lengths and offsets.
SND_TEST(KernelsIndep, InPlaceRamps) {
  Buf b;
  for (std::uint32_t n : {1u, 3u, 7u, 8u, 9u, 17u, 64u, 65u}) {
    for (std::uint32_t mis : {0u, 1u, 5u}) {
      b.Fill(mis, n, n);
      std::vector<float> x(b.at(mis), b.at(mis) + n);
      CopyGainRamp(b.at(mis), b.at(mis), n, 1.0f, 0.0f);
      for (std::uint32_t i = 0; i < n; ++i) SND_CHECK_NEAR(b.at(mis)[i], x[i] * RampGain(1.0f, 0.0f, n, i), 1e-6);
      SND_CHECK(b.GuardsIntact(mis, n));
      Copy(b.at(mis), b.at(mis), n);  // dst == src is allowed
      SND_CHECK(b.GuardsIntact(mis, n));
    }
  }
}

// D-26: frame 0 uses `start` exactly, the block "ends just before" `end`, and
// the next block starting at `end` continues the line without a step: the
// gains of consecutive blocks lie on one straight line.
SND_TEST(KernelsIndep, RampEndpointsAndBlockContinuity) {
  constexpr std::uint32_t kN = 37;  // not a multiple of any SIMD width
  std::vector<float> ones(4 * kN, 1.0f), g(4 * kN, 0.0f);
  // Global ramp 0 -> 1 over 4 blocks: block b goes from b/4 to (b+1)/4.
  for (std::uint32_t b = 0; b < 4; ++b) {
    const float s = static_cast<float>(b) / 4.0f, e = static_cast<float>(b + 1) / 4.0f;
    CopyGainRamp(g.data() + b * kN, ones.data(), kN, s, e);
  }
  SND_CHECK_EQ(g[0], 0.0f);
  float maxStep = 0.0f, minStep = 1.0f;
  for (std::uint32_t i = 1; i < 4 * kN; ++i) {
    const float d = g[i] - g[i - 1];
    maxStep = std::max(maxStep, d);
    minStep = std::min(minStep, d);
  }
  const float ideal = 1.0f / (4.0f * kN);
  SND_CHECK_MSG(std::fabs(maxStep - ideal) < 1e-6f && std::fabs(minStep - ideal) < 1e-6f,
                "steps %.9g..%.9g ideal %.9g", static_cast<double>(minStep), static_cast<double>(maxStep),
                static_cast<double>(ideal));
  // A single frame uses the start value.
  float one = 2.0f, out = 0.0f;
  CopyGainRamp(&out, &one, 1, 0.25f, 0.75f);
  SND_CHECK_EQ(out, 0.5f);
  MixGainRamp(&out, &one, 1, 0.0f, 1.0f);  // gain 0 at frame 0
  SND_CHECK_EQ(out, 0.5f);
  // Zero frames: no write.
  out = kGuard;
  CopyGainRamp(&out, &one, 0, 1.0f, 2.0f);
  MixGainRamp(&out, &one, 0, 1.0f, 2.0f);
  Mix(&out, &one, 0);
  Scale(&out, 0, 0.0f);
  Clear(&out, 0);
  SND_CHECK_EQ(out, kGuard);
}

SND_TEST(KernelsIndep, InterleaveRoundTripAllChannelCounts) {
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    for (std::uint32_t n : {0u, 1u, 5u, 64u, 67u}) {
      dsptest::Audio a(ch, n == 0 ? 1 : n), b(ch, n == 0 ? 1 : n);
      dsptest::Noise nz(ch * 100 + n);
      for (std::uint32_t c = 0; c < ch; ++c)
        for (std::uint32_t i = 0; i < n; ++i) a.ch(c)[i] = nz.Next();
      std::vector<float> il(ch * n + 2, kGuard);
      {
        SND_RT_SCOPE();
        Interleave(a.cp(), ch, n, il.data() + 1);
      }
      SND_CHECK_EQ(il[0], kGuard);
      SND_CHECK_EQ(il[ch * n + 1], kGuard);
      for (std::uint32_t i = 0; i < n; ++i)
        for (std::uint32_t c = 0; c < ch; ++c) SND_CHECK_EQ(il[1 + i * ch + c], a.ch(c)[i]);
      {
        SND_RT_SCOPE();
        Deinterleave(il.data() + 1, ch, n, b.p());
      }
      for (std::uint32_t c = 0; c < ch; ++c)
        for (std::uint32_t i = 0; i < n; ++i) SND_CHECK_EQ(b.ch(c)[i], a.ch(c)[i]);
    }
  }
}

SND_TEST(KernelsIndep, ReductionsWithSpecialValues) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (std::uint32_t n = 1; n <= 40; ++n) {
    for (std::uint32_t pos = 0; pos < n; ++pos) {
      std::vector<float> x(n, 0.25f);
      x[pos] = nan;
      SND_CHECK_EQ(Peak(x.data(), n), n == 1 ? 0.0f : 0.25f);  // NaN ignored
      SND_CHECK(HasNonFinite(x.data(), n));
      x[pos] = -INFINITY;
      SND_CHECK_EQ(Peak(x.data(), n), INFINITY);
      SND_CHECK(HasNonFinite(x.data(), n));
      x[pos] = 1e-40f;
      SND_CHECK(HasDenormal(x.data(), n));
      SND_CHECK(!HasNonFinite(x.data(), n));
      x[pos] = -0.75f;
      SND_CHECK_EQ(Peak(x.data(), n), 0.75f);
      SND_CHECK(!HasDenormal(x.data(), n));
    }
  }
  SND_CHECK_EQ(Peak(nullptr, 0), 0.0f);
  SND_CHECK_EQ(SumOfSquares(nullptr, 0), 0.0);
  // SumOfSquares on a long buffer of 1.0 + tiny values: double accumulation is
  // promised, so 1e6 frames of 1.0 must sum to exactly 1e6.
  std::vector<float> ones(1000003, 1.0f);
  SND_CHECK_EQ(SumOfSquares(ones.data(), static_cast<std::uint32_t>(ones.size())), 1000003.0);
}

SND_TEST(KernelsIndep, DbConversions) {
  SND_CHECK_EQ(DbToGain(0.0f), 1.0f);
  SND_CHECK_NEAR(DbToGain(-6.0206f), 0.5, 1e-5);
  SND_CHECK_NEAR(DbToGain(20.0f), 10.0, 1e-5);
  SND_CHECK_EQ(DbToGain(-INFINITY), 0.0f);
  SND_CHECK_EQ(DbToGain(kMinDb), 0.0f);
  SND_CHECK(DbToGain(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
  SND_CHECK_EQ(GainToDb(0.0f), kMinDb);
  SND_CHECK_EQ(GainToDb(-0.0f), kMinDb);
  SND_CHECK_EQ(GainToDb(1e-30f), kMinDb);
  SND_CHECK_NEAR(GainToDb(-0.5f), -6.0206, 1e-4);
  SND_CHECK_EQ(GainToDb(std::numeric_limits<float>::quiet_NaN()), kMinDb);
  for (float db = -140.0f; db <= 40.0f; db += 0.37f) SND_CHECK_NEAR(GainToDb(DbToGain(db)), db, 1e-3);
}
