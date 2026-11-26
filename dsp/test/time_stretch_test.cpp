// WSOLA time stretch (time_stretch.hpp, D-209 / D-210): speed 1 is the input bit
// for bit, lengths follow the speed, the pitch does not change, ramps, block-split
// invariance, multichannel coherence, RT safety, cost.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/time_stretch.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr std::uint32_t kRate = 48000;

using Signal = std::vector<std::vector<float>>;  // [channel][frame]

struct Stretcher {
  dsptest::AlignedMem mem;
  std::uint32_t ch;
  std::uint64_t pos = 0;  // next unread frame of the source
  Stretcher(std::uint32_t channels, std::uint32_t rate = kRate, float maxSpeed = kTimeStretchMaxSpeed)
      : mem(TimeStretchStateBytes(channels, rate, maxSpeed), dsptest::kDirty), ch(channels) {
    ok = TimeStretchInit(mem.get(), mem.size(), channels, rate, maxSpeed);
  }
  bool ok = false;
  void* st() const { return mem.get(); }
  // One call: pulls the frames it needs from `src` (zeros past its end), appends to `out`.
  std::uint32_t Call(const Signal& src, Signal* out, std::uint32_t frames, float s0, float s1) {
    const std::uint32_t n = TimeStretchInputFrames(st(), frames, s0, s1);
    const std::uint32_t pad = TimeStretchPadFrames(st());
    std::vector<std::vector<float>> inb(ch, std::vector<float>(pad + n + 1u, 7.0f));
    std::vector<std::vector<float>> ob(ch, std::vector<float>(frames + 1u, 0.0f));
    float* ip[8];
    float* op[8];
    for (std::uint32_t c = 0; c < ch; ++c) {
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint64_t at = pos + i;
        inb[c][pad + i] = at < src[c].size() ? src[c][at] : 0.0f;
      }
      ip[c] = inb[c].data();
      op[c] = ob[c].data();
    }
    std::uint32_t used;
    {
      SND_RT_SCOPE();
      used = TimeStretchProcess(st(), ip, n, op, frames, s0, s1);
    }
    pos += used;
    for (std::uint32_t c = 0; c < ch; ++c) (*out)[c].insert((*out)[c].end(), ob[c].begin(), ob[c].begin() + frames);
    return used;
  }
};

Signal NoiseSignal(std::uint32_t ch, std::uint32_t frames, std::uint32_t seed) {
  Signal s(ch, std::vector<float>(frames));
  for (std::uint32_t c = 0; c < ch; ++c) {
    dsptest::Noise nz(seed + c * 17u);
    for (float& v : s[c]) v = nz.Next(0.5f);
  }
  return s;
}

Signal Tone(std::uint32_t ch, std::uint32_t frames, double hz, double phase = 0.0) {
  Signal s(ch, std::vector<float>(frames));
  for (std::uint32_t c = 0; c < ch; ++c)
    for (std::uint32_t i = 0; i < frames; ++i)
      s[c][i] = static_cast<float>(0.5 * std::sin(2.0 * dsptest::kPi * hz * i / kRate + phase));
  return s;
}

// Strongest frequency in [lo, hi] (0.25 Hz steps) of x[from, from + n).
double PeakHz(const std::vector<float>& x, std::uint32_t from, std::uint32_t n, double lo, double hi) {
  double best = lo, bestA = -1.0;
  for (double f = lo; f <= hi; f += 0.25) {
    const double a = dsptest::ToneAmplitude(x.data() + from, n, f, kRate);
    if (a > bestA) {
      bestA = a;
      best = f;
    }
  }
  return best;
}

}  // namespace

SND_TEST(TimeStretch, ShapeAndInvalidArguments) {
  TimeStretchShape sh{};
  SND_REQUIRE(TimeStretchGetShape(kRate, 4.0f, &sh));
  SND_CHECK_EQ(sh.hopFrames, 720u);  // 30 ms window
  SND_CHECK_EQ(sh.searchFrames, 576u);  // 12 ms (D-217)
  SND_CHECK_EQ(sh.decimation, 8u);
  SND_CHECK_EQ(sh.lookaheadFrames, 576u + 1440u);
  SND_CHECK(sh.padFrames % 16u == 0u && sh.padFrames >= 2u * 576u + 720u + 4u * 720u + 2u);
  // NaN maxSpeed is 1 (D-217).
  TimeStretchShape one{}, nanShape{};
  SND_REQUIRE(TimeStretchGetShape(kRate, 1.0f, &one));
  SND_REQUIRE(TimeStretchGetShape(kRate, std::nanf(""), &nanShape));
  SND_CHECK_EQ(nanShape.padFrames, one.padFrames);
  SND_CHECK_EQ(TimeStretchStateBytes(0, kRate), 0u);
  SND_CHECK_EQ(TimeStretchStateBytes(9, kRate), 0u);
  SND_CHECK_EQ(TimeStretchStateBytes(2, 0), 0u);
  SND_CHECK_EQ(TimeStretchStateBytes(2, kTimeStretchMaxRate + 1u), 0u);
  const std::size_t b = TimeStretchStateBytes(2, kRate);
  SND_CHECK(b > 0u && b % 64u == 0u);
  dsptest::AlignedMem m(b);
  SND_CHECK(!TimeStretchInit(m.get(), b - 64u, 2, kRate));
  SND_CHECK(TimeStretchInit(m.get(), b, 2, kRate));
  // A smaller maxSpeed keeps less history.
  SND_CHECK(TimeStretchStateBytes(2, kRate, 1.5f) < b);
  snd::test::Note("state: stereo 48 kHz max speed 4: %u bytes, pad %u frames", static_cast<unsigned>(b), sh.padFrames);
}

// Speed 1 from Init: output == input bit for bit, no delay, however the calls are split.
SND_TEST(TimeStretch, SpeedOneIsBitExact) {
  const Signal src = NoiseSignal(2, 48000, 5);
  for (const std::vector<std::uint32_t>& sizes :
       {std::vector<std::uint32_t>{256}, std::vector<std::uint32_t>{1, 100, 997, 64, 4096}}) {
    Stretcher t(2);
    SND_REQUIRE(t.ok);
    Signal out(2);
    std::uint32_t total = 0, k = 0;
    while (total < 40000u) {
      const std::uint32_t n = sizes[k++ % sizes.size()];
      t.Call(src, &out, n, 1.0f, 1.0f);
      total += n;
    }
    bool same = true;
    for (std::uint32_t c = 0; c < 2; ++c)
      for (std::uint32_t i = 0; i < total; ++i) same &= out[c][i] == src[c][i];
    SND_CHECK_MSG(same, "speed 1 is not the input (sizes %u)", static_cast<unsigned>(sizes.size()));
    // Consumed = output + look-ahead (within one hop).
    const double off = TimeStretchPendingOffset(t.st());
    SND_CHECK_EQ(static_cast<double>(TimeStretchConsumedFrames(t.st())) + off, static_cast<double>(total));
  }
}

// Output length vs input consumed: consumed ~= speed * output + look-ahead (+- one hop).
SND_TEST(TimeStretch, LengthFollowsSpeed) {
  const Signal src = NoiseSignal(1, 48000 * 12, 9);
  for (float speed : {0.25f, 0.5f, 0.8f, 1.25f, 2.0f, 4.0f}) {
    Stretcher t(1);
    SND_REQUIRE(t.ok);
    Signal out(1);
    const std::uint32_t outFrames = 48000 * 2;
    for (std::uint32_t done = 0; done < outFrames; done += 480) t.Call(src, &out, 480, speed, speed);
    const double consumed = static_cast<double>(TimeStretchConsumedFrames(t.st()));
    const double expect = speed * outFrames + TimeStretchLookaheadFrames(t.st());
    const double hop = TimeStretchHopFrames(t.st());
    SND_CHECK_MSG(std::fabs(consumed - expect) <= speed * hop + 2.0, "speed %.2f: consumed %.0f, expected %.0f",
                  static_cast<double>(speed), consumed, expect);
    SND_CHECK(TimeStretchInputFrames(t.st(), 480, speed, speed) <= TimeStretchMaxInputFrames(t.st(), 480));
  }
}

// The pitch stays: a tone keeps its frequency at speeds 0.5 .. 2, and nothing
// appears at the frequency a resampler would have produced.
SND_TEST(TimeStretch, PitchIsPreserved) {
  for (double hz : {220.0, 440.0, 1000.0}) {
    const Signal src = Tone(1, 48000 * 10, hz);
    for (float speed : {0.5f, 0.75f, 1.5f, 2.0f}) {
      Stretcher t(1);
      SND_REQUIRE(t.ok);
      Signal out(1);
      for (std::uint32_t done = 0; done < 48000u * 2u; done += 256) t.Call(src, &out, 256, speed, speed);
      const std::uint32_t from = 48000 / 2, n = 16384;
      const double peak = PeakHz(out[0], from, n, hz * 0.9, hz * 1.1);
      const double aTone = dsptest::ToneAmplitude(out[0].data() + from, n, hz, kRate);
      const double aShift = dsptest::ToneAmplitude(out[0].data() + from, n, hz * speed, kRate);
      SND_CHECK_MSG(std::fabs(peak - hz) <= 1.0, "%.0f Hz at speed %.2f: peak %.2f Hz", hz, static_cast<double>(speed), peak);
      SND_CHECK_MSG(aTone > 0.35 && aShift < 0.05 * aTone, "%.0f Hz at speed %.2f: amp %.3f, at %.0f Hz %.4f", hz,
                    static_cast<double>(speed), aTone, hz * speed, aShift);
    }
  }
}

// A ramp 0.5 -> 2 over 4 s (blocks with matching intermediate speeds): the input
// consumed follows the integral of the speed and the pitch stays.
SND_TEST(TimeStretch, RampFollowsTheIntegral) {
  const Signal src = Tone(1, 48000 * 12, 500.0);
  Stretcher t(1);
  SND_REQUIRE(t.ok);
  Signal out(1);
  const std::uint32_t total = 48000 * 4, blk = 256;
  double integral = 0.0;
  for (std::uint32_t done = 0; done < total; done += blk) {
    const float s0 = 0.5f + 1.5f * static_cast<float>(done) / total;
    const float s1 = 0.5f + 1.5f * static_cast<float>(done + blk) / total;
    t.Call(src, &out, blk, s0, s1);
    integral += 0.5 * (s0 + s1) * blk;
  }
  const double consumed = static_cast<double>(TimeStretchConsumedFrames(t.st()));
  const double expect = integral + TimeStretchLookaheadFrames(t.st());
  SND_CHECK_MSG(std::fabs(consumed - expect) <= 2.0 * TimeStretchHopFrames(t.st()), "ramp: consumed %.0f, expected %.0f",
                consumed, expect);
  for (std::uint32_t from : {48000u / 2u, 48000u * 2u, 48000u * 3u}) {
    const double peak = PeakHz(out[0], from, 8192, 450.0, 550.0);
    SND_CHECK_MSG(std::fabs(peak - 500.0) <= 2.0, "ramp at %u: peak %.2f Hz", from, peak);
  }
}

// Constant speed: bit-identical however the frames are split into calls. A ramp
// split at the matching intermediate speeds: the same up to rounding.
SND_TEST(TimeStretch, BlockSplitInvariance) {
  const Signal src = NoiseSignal(2, 48000 * 4, 21);
  for (float speed : {0.63f, 1.37f, 3.1f}) {
    Stretcher a(2), b(2);
    Signal oa(2), ob(2);
    a.Call(src, &oa, 20000, speed, speed);
    dsptest::Noise nz(3);
    std::uint32_t done = 0;
    while (done < 20000u) {
      std::uint32_t n = 1u + static_cast<std::uint32_t>((nz.Next(1.0f) + 1.0f) * 700.0f);
      if (n > 20000u - done) n = 20000u - done;
      b.Call(src, &ob, n, speed, speed);
      done += n;
    }
    bool same = a.pos == b.pos;
    for (std::uint32_t c = 0; c < 2; ++c)
      for (std::uint32_t i = 0; i < 20000u; ++i) same &= oa[c][i] == ob[c][i];
    SND_CHECK_MSG(same, "speed %.2f: split output differs", static_cast<double>(speed));
  }
  {
    Stretcher a(1), b(1);
    Signal oa(1), ob(1);
    a.Call(src, &oa, 8192, 0.7f, 1.9f);
    for (std::uint32_t q = 0; q < 4; ++q) {
      const float s0 = 0.7f + 1.2f * (static_cast<float>(q) / 4.0f);
      const float s1 = 0.7f + 1.2f * (static_cast<float>(q + 1) / 4.0f);
      b.Call(src, &ob, 2048, s0, s1);
    }
    const float d = dsptest::MaxAbsDiff(oa[0].data(), ob[0].data(), 8192);
    snd::test::Note("ramp 0.7 -> 1.9 in one call vs 4: max diff %.3g, consumed %.0f / %.0f", static_cast<double>(d),
                    static_cast<double>(a.pos), static_cast<double>(b.pos));
    SND_CHECK(a.pos == b.pos);
    SND_CHECK(d < 1e-6f);
  }
}

// Every channel uses the same splice positions: a channel that is an exact scaled
// copy stays an exact scaled copy, and the phase between quadrature channels stays.
SND_TEST(TimeStretch, ChannelsStayCoherent) {
  for (float speed : {0.6f, 1.8f}) {
    Signal src = NoiseSignal(4, 48000 * 4, 33);
    for (std::uint32_t i = 0; i < src[0].size(); ++i) src[1][i] = -0.5f * src[0][i];
    Stretcher t(4);
    Signal out(4);
    for (std::uint32_t done = 0; done < 48000u; done += 300) t.Call(src, &out, 300, speed, speed);
    bool scaled = true;
    for (std::uint32_t i = 0; i < out[0].size(); ++i) scaled &= out[1][i] == -0.5f * out[0][i];
    SND_CHECK_MSG(scaled, "speed %.2f: scaled copy broken", static_cast<double>(speed));
    // Quadrature pair: L = sin, R = cos at 300 Hz -> phase difference 90 degrees.
    Signal q(2, std::vector<float>(48000 * 4));
    for (std::uint32_t i = 0; i < q[0].size(); ++i) {
      const double a = 2.0 * dsptest::kPi * 300.0 * i / kRate;
      q[0][i] = static_cast<float>(0.5 * std::sin(a));
      q[1][i] = static_cast<float>(0.5 * std::cos(a));
    }
    Stretcher t2(2);
    Signal o2(2);
    for (std::uint32_t done = 0; done < 48000u; done += 256) t2.Call(q, &o2, 256, speed, speed);
    double worst = 0.0;
    for (std::uint32_t from = 4800; from + 1600 < 48000u; from += 1600) {
      double rl = 0, il = 0, rr = 0, ir = 0;
      for (std::uint32_t i = 0; i < 1600; ++i) {  // 10 periods
        const double a = 2.0 * dsptest::kPi * 300.0 * i / kRate;
        rl += o2[0][from + i] * std::cos(a);
        il -= o2[0][from + i] * std::sin(a);
        rr += o2[1][from + i] * std::cos(a);
        ir -= o2[1][from + i] * std::sin(a);
      }
      double d = std::atan2(ir, rr) - std::atan2(il, rl);
      while (d > dsptest::kPi) d -= 2 * dsptest::kPi;
      while (d < -dsptest::kPi) d += 2 * dsptest::kPi;
      worst = std::max(worst, std::fabs(std::fabs(d) - dsptest::kPi / 2.0));
    }
    SND_CHECK_MSG(worst < 0.02, "speed %.2f: quadrature phase off by %.4f rad", static_cast<double>(speed), worst);
  }
}

// Too little input: silence, state unchanged. Reset == fresh Init.
SND_TEST(TimeStretch, ShortInputAndReset) {
  const Signal src = NoiseSignal(2, 48000, 41);
  Stretcher a(2), fresh(2);
  Signal oa(2), of(2);
  for (int b = 0; b < 20; ++b) a.Call(src, &oa, 256, 1.6f, 1.6f);
  {
    const std::uint32_t pad = TimeStretchPadFrames(a.st());
    const std::uint32_t need = TimeStretchInputFrames(a.st(), 4096, 1.6f, 1.6f);
    SND_REQUIRE(need > 0u);
    std::vector<float> b0(pad + need, 0.1f), b1(pad + need, 0.1f), o0(4096, 9.0f), o1(4096, 9.0f);
    float* ip[2] = {b0.data(), b1.data()};
    float* op[2] = {o0.data(), o1.data()};
    const std::uint64_t before = TimeStretchConsumedFrames(a.st());
    SND_RT_SCOPE();
    SND_CHECK_EQ(TimeStretchProcess(a.st(), ip, need - 1u, op, 4096, 1.6f, 1.6f), 0u);
    SND_CHECK_EQ(TimeStretchConsumedFrames(a.st()), before);
    SND_CHECK(dsptest::Rms(o0.data(), 4096) == 0.0 && dsptest::Rms(o1.data(), 4096) == 0.0);
    TimeStretchReset(a.st());
  }
  a.pos = 0;
  oa.assign(2, {});
  for (int b = 0; b < 30; ++b) {
    a.Call(src, &oa, 333, 0.7f, 0.9f);
    fresh.Call(src, &of, 333, 0.7f, 0.9f);
  }
  bool same = true;
  for (std::uint32_t c = 0; c < 2; ++c) same &= oa[c] == of[c];
  SND_CHECK(same);
}

// Cost per voice and 256 output frames (stereo, speed 1.3 = searching every hop).
SND_TEST(TimeStretch, Cost) {
  const Signal src = NoiseSignal(2, 48000 * 20, 51);
  for (float speed : {1.0f, 0.8f, 1.3f, 2.5f}) {
    Stretcher t(2);
    Signal out(2);
    const std::uint32_t pad = TimeStretchPadFrames(t.st());
    const std::uint32_t maxIn = TimeStretchMaxInputFrames(t.st(), 256);
    std::vector<float> b0(pad + maxIn), b1(pad + maxIn), o0(256), o1(256);
    float* ip[2] = {b0.data(), b1.data()};
    float* op[2] = {o0.data(), o1.data()};
    std::uint64_t pos = 0;
#ifdef NDEBUG
    const int blocks = 1500;
#else
    const int blocks = 150;
#endif
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
      const std::uint32_t n = TimeStretchInputFrames(t.st(), 256, speed, speed);
      for (std::uint32_t i = 0; i < n; ++i) {
        b0[pad + i] = src[0][(pos + i) % src[0].size()];
        b1[pad + i] = src[1][(pos + i) % src[1].size()];
      }
      pos += TimeStretchProcess(t.st(), ip, n, op, 256, speed, speed);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    snd::test::Note("time stretch stereo speed %.1f: %.2f us per 256 frames (incl. copying the input)",
                    static_cast<double>(speed), us);
  }
}

// D-217: the search sums the per-channel correlations: anti-phase stereo (R = -L)
// is stretched exactly like the mono signal, a silent channel changes nothing.
SND_TEST(TimeStretch, PerChannelCorrelation) {
  const Signal mono = Tone(1, 48000 * 4, 220.0);
  Signal anti(2, mono[0]), half(2, mono[0]);
  for (float& v : anti[1]) v = -v;
  for (float& v : half[1]) v = 0.0f;
  for (float speed : {0.7f, 1.5f}) {
    Stretcher m(1), a(2), h(2);
    Signal om(1), oa(2), oh(2);
    for (std::uint32_t done = 0; done < 48000u * 2u; done += 256) {
      m.Call(mono, &om, 256, speed, speed);
      a.Call(anti, &oa, 256, speed, speed);
      h.Call(half, &oh, 256, speed, speed);
    }
    SND_CHECK_MSG(oa[0] == om[0] && oh[0] == om[0], "speed %.1f: stereo choice differs from mono", static_cast<double>(speed));
    bool neg = true;
    for (std::size_t i = 0; i < oa[0].size(); ++i) neg &= oa[1][i] == -oa[0][i];
    SND_CHECK(neg);
  }
}

// D-217: ramp endpoints are clamped before the interpolation (no Inf - Inf).
SND_TEST(TimeStretch, InfiniteEndpointsAreClampedFirst) {
  const Signal src = NoiseSignal(1, 48000 * 4, 61);
  Stretcher a(1, kRate, 2.0f), b(1, kRate, 2.0f);
  Signal oa(1), ob(1);
  for (int k = 0; k < 40; ++k) {
    a.Call(src, &oa, 256, INFINITY, -INFINITY);
    b.Call(src, &ob, 256, 2.0f, 0.25f);
  }
  SND_CHECK(oa[0] == ob[0] && a.pos == b.pos);
}

// D-217: one call handles at most kTimeStretchMaxCallFrames; the input count fits uint32.
SND_TEST(TimeStretch, CallSizeIsCapped) {
  Stretcher t(1);
  const std::uint32_t n = TimeStretchInputFrames(t.st(), 0xFFFFFFFFu, 4.0f, 4.0f);
  SND_CHECK_EQ(n, TimeStretchInputFrames(t.st(), kTimeStretchMaxCallFrames, 4.0f, 4.0f));
  SND_CHECK(n <= TimeStretchMaxInputFrames(t.st(), kTimeStretchMaxCallFrames));
  SND_CHECK(n > 4u * (kTimeStretchMaxCallFrames - 720u));
}

// Low frequencies (D-217, search +-12 ms): residual against a fitted sine per 20 ms window.
SND_TEST(TimeStretch, LowFrequencyResidual) {
  for (double hz : {50.0, 55.0, 73.0, 100.0, 220.0}) {
    const Signal src = Tone(1, 48000 * 12, hz);
    double worstAll = 0.0;
    for (float speed : {0.25f, 0.5f, 0.8f, 1.25f, 1.5f, 2.0f}) {
      Stretcher t(1);
      Signal out(1);
      for (std::uint32_t done = 0; done < 48000u * 3u; done += 256) t.Call(src, &out, 256, speed, speed);
      double worst = 0.0;
      const std::uint32_t w = 960;
      for (std::uint32_t i = 9600; i + w <= 48000u * 3u; i += w) {
        double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
        for (std::uint32_t j = 0; j < w; ++j) {
          const double a = 2.0 * dsptest::kPi * hz * (i + j) / kRate;
          ss += std::sin(a) * std::sin(a);
          cc += std::cos(a) * std::cos(a);
          sc += std::sin(a) * std::cos(a);
          xs += out[0][i + j] * std::sin(a);
          xc += out[0][i + j] * std::cos(a);
        }
        const double det = ss * cc - sc * sc;
        const double A = (xs * cc - xc * sc) / det, B = (xc * ss - xs * sc) / det;
        double r = 0.0;
        for (std::uint32_t j = 0; j < w; ++j) {
          const double a = 2.0 * dsptest::kPi * hz * (i + j) / kRate;
          const double d = out[0][i + j] - (A * std::sin(a) + B * std::cos(a));
          r += d * d;
        }
        worst = std::max(worst, std::sqrt(2.0 * r / w) / std::max(1e-9, std::hypot(A, B)));
      }
      worstAll = std::max(worstAll, worst);
      snd::test::Note("%.0f Hz speed %.2f: worst residual %.4f", hz, static_cast<double>(speed), worst);
    }
    SND_CHECK_MSG(worstAll < 0.05, "%.0f Hz: worst residual %.4f", hz, worstAll);
  }
}
