// Defects found by the independent tests of stage 7-D dsp (FFT, time stretch,
// convolution). Every Defect_ test FAILS on the current implementation and
// passes once the defect is fixed. They are a separate executable
// (dsp_indep_defects_dsp_test) so the other independent tests stay green.
// Not to be edited by the implementer (fix the code, not the expectation).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/convolution.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/time_stretch.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr double kPi = dsptest::kPi;
constexpr std::uint32_t kRate = 48000;
using Signal = std::vector<std::vector<float>>;

// Runs a time stretcher over `src` in 256-frame blocks at a constant speed.
Signal Stretch(const Signal& src, std::uint32_t frames, float speed, float maxSpeed = 4.0f) {
  const std::uint32_t ch = static_cast<std::uint32_t>(src.size());
  dsptest::AlignedMem mem(TimeStretchStateBytes(ch, kRate, maxSpeed), dsptest::kDirty);
  Signal out(ch);
  if (!TimeStretchInit(mem.get(), mem.size(), ch, kRate, maxSpeed)) return out;
  const std::uint32_t pad = TimeStretchPadFrames(mem.get());
  std::uint64_t pos = 0;
  for (std::uint32_t done = 0; done < frames; done += 256u) {
    const std::uint32_t n = TimeStretchInputFrames(mem.get(), 256, speed, speed);
    std::vector<std::vector<float>> in(ch, std::vector<float>(pad + n)), o(ch, std::vector<float>(256));
    float* ip[8];
    float* op[8];
    for (std::uint32_t c = 0; c < ch; ++c) {
      for (std::uint32_t i = 0; i < n; ++i) in[c][pad + i] = pos + i < src[c].size() ? src[c][pos + i] : 0.0f;
      ip[c] = in[c].data();
      op[c] = o[c].data();
    }
    pos += TimeStretchProcess(mem.get(), ip, n, op, 256, speed, speed);
    for (std::uint32_t c = 0; c < ch; ++c) out[c].insert(out[c].end(), o[c].begin(), o[c].end());
  }
  return out;
}

// Worst residual (relative to the amplitude) of x against a fitted sinusoid at f, per window w.
double WorstResidual(const std::vector<float>& x, std::uint32_t from, std::uint32_t to, double f, std::uint32_t w) {
  double worst = 0.0;
  for (std::uint32_t i = from; i + w <= to; i += w) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (std::uint32_t j = 0; j < w; ++j) {
      const double a = 2.0 * kPi * f * (i + j) / kRate;
      ss += std::sin(a) * std::sin(a);
      cc += std::cos(a) * std::cos(a);
      sc += std::sin(a) * std::cos(a);
      xs += x[i + j] * std::sin(a);
      xc += x[i + j] * std::cos(a);
    }
    const double det = ss * cc - sc * sc;
    const double A = (xs * cc - xc * sc) / det, B = (xc * ss - xs * sc) / det;
    double r = 0.0;
    for (std::uint32_t j = 0; j < w; ++j) {
      const double a = 2.0 * kPi * f * (i + j) / kRate;
      const double d = x[i + j] - (A * std::sin(a) + B * std::cos(a));
      r += d * d;
    }
    worst = std::max(worst, std::sqrt(2.0 * r / w) / std::max(1e-9, std::hypot(A, B)));
  }
  return worst;
}

}  // namespace

// D-210: "maxSpeed は Init で 1〜4（既定 4、NaN は 1 として扱う）". The
// implementation treats a NaN maxSpeed as 4 (time_stretch.cpp Shape / Init:
// ClampMs(maxSpeed, 1, 4, kTimeStretchMaxSpeed)): the pad and the state are
// sized for speed 4 and speed 2 is not clamped to 1.
SND_TEST(IndepDefects, Defect_TimeStretchNanMaxSpeedIsOne) {
  TimeStretchShape a{}, one{};
  SND_REQUIRE(TimeStretchGetShape(kRate, std::nanf(""), &a));
  SND_REQUIRE(TimeStretchGetShape(kRate, 1.0f, &one));
  SND_CHECK_EQ(a.padFrames, one.padFrames);
  SND_CHECK_EQ(TimeStretchStateBytes(2, kRate, std::nanf("")), TimeStretchStateBytes(2, kRate, 1.0f));
  Signal src(1, std::vector<float>(48000 * 4));
  dsptest::Noise nz(3);
  for (float& v : src[0]) v = nz.Next(0.5f);
  const Signal x = Stretch(src, 24000, 2.0f, std::nanf(""));
  const Signal y = Stretch(src, 24000, 2.0f, 1.0f);
  SND_CHECK_MSG(x[0] == y[0], "speed 2 on a NaN-maxSpeed instance is not clamped to 1");
}

// D-209 / time_stretch.hpp: all channels share p_k, chosen by the normalised
// cross-correlation "channels summed". The implementation correlates the SUM
// of the channel signals (time_stretch.cpp Search, lines 155-164). For a stereo
// signal whose channels cancel in the sum (R = -L, a common polarity-inverted
// or wide stereo case) every candidate scores 0 and the lowest position is
// always taken, so every hop is spliced at a random phase. The left channel of
// such a stereo pair must be stretched as well as the same signal in mono
// (summing per-channel correlations, or correlating sum(|.|) / each channel,
// gives the same choice as mono here).
SND_TEST(IndepDefects, Defect_TimeStretchAntiPhaseStereoLosesAlignment) {
  const double hz = 220.0;
  Signal mono(1, std::vector<float>(48000 * 6));
  for (std::uint32_t i = 0; i < mono[0].size(); ++i)
    mono[0][i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * i / kRate));
  Signal st(2, mono[0]);
  for (float& v : st[1]) v = -v;
  for (float speed : {0.7f, 1.5f}) {
    const Signal m = Stretch(mono, 96000, speed);
    const Signal s = Stretch(st, 96000, speed);
    const double rm = WorstResidual(m[0], 9600, 96000, hz, 960);
    const double rs = WorstResidual(s[0], 9600, 96000, hz, 960);
    snd::test::Note("220 Hz x%.1f: worst residual mono %.4f, anti-phase stereo L %.4f", speed, rm, rs);
    SND_CHECK_MSG(rs < 2.0 * rm + 0.02, "speed %g: anti-phase stereo residual %g vs mono %g", speed, rs, rm);
  }
}

namespace {

std::vector<float> DecayNoise(std::uint32_t n, std::uint32_t seed, double tau) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (std::uint32_t i = 0; i < n; ++i) v[i] = static_cast<float>(nz.Next(1.0f) * 0.4 * std::exp(-static_cast<double>(i) / tau));
  return v;
}

struct PreparedIr {
  dsptest::AlignedMem mem;
  const ConvolutionIr* ir = nullptr;
  std::uint32_t slot = 0;
  PreparedIr(const std::vector<float>& h, std::uint32_t b) {
    const float* p[1] = {h.data()};
    const ConvolutionIrDesc d{p, 1, static_cast<std::uint32_t>(h.size()), kRate, 1.0f};
    const std::size_t bytes = ConvolutionIrBytes(d, kRate, b);
    mem.Reset(bytes);
    ir = ConvolutionPrepareIr(mem.get(), bytes, d, kRate, b);
    if (ir != nullptr) slot = ConvolutionRegisterIr(ir);
  }
  ~PreparedIr() {
    if (slot != 0u) ConvolutionUnregisterIr(slot);
  }
};

struct ConvFx {
  dsptest::AlignedMem mem;
  float params[kMaxEffectParams] = {};
  snd::EffectDesc desc{};
  ConvFx(std::uint32_t slot, std::uint32_t b) {
    EffectDefaultParams(EffectKind::Convolution, params);
    params[kConvIrSlot] = static_cast<float>(slot);
    params[kConvPartitionFrames] = static_cast<float>(b);
    params[kConvMaxIrMs] = 100.0f;
    desc.effect_type = kEffectConvolution;
    desc.channels = 1;
    desc.max_frames = 256;
    desc.param_count = kConvParamCount;
    desc.initial_params = params;
    mem.Reset(EffectStateBytes(desc, kRate), dsptest::kDirty);
    EffectInit(mem.get(), mem.size(), desc, kRate);
  }
  void Process(float* buf, std::uint32_t n, float slotEnd) {
    float s[1] = {slotEnd}, e[1] = {slotEnd};
    snd::ParamBlock pb{s, e, 1, 1, nullptr};
    float* p[1] = {buf};
    EffectProcess(mem.get(), p, p, 1, n, &pb, nullptr);
  }
};

}  // namespace

// D-215 / convolution.hpp: "Changing IrSlot swaps the IR at the call boundary;
// the input history (FDL) is kept, so the new IR applies to the recent input at
// once". The direct head switches at the call boundary, but the FDL output of
// the partition period in progress was computed at the end of the previous
// period with the OLD IR (fx_convolution.cpp Step writes OutBuf for the next
// period; ConvolutionProcess adds it without re-resolving). So for up to B
// frames after the switch the output is head(new IR) + tail(old IR); after an
// unregister the old IR's tail is still heard for up to B frames. Calls aligned
// to B (block 256, B 64): the first 64 frames after the switch are wrong.
SND_TEST(IndepDefects, Defect_ConvolutionIrSwitchLagsOnePartition) {
  const std::uint32_t b = 64;
  const std::vector<float> h1 = DecayNoise(600, 1, 150.0), h2 = DecayNoise(600, 2, 150.0);
  PreparedIr a(h1, b), c(h2, b);
  SND_REQUIRE(a.slot != 0u && c.slot != 0u);
  ConvFx fx(a.slot, b);
  SND_REQUIRE(EffectStateKind(fx.mem.get()) == EffectKind::Convolution);
  std::vector<float> x(2048);
  dsptest::Noise nz(4);
  for (float& v : x) v = nz.Next(0.5f);
  std::vector<float> y(x);
  fx.Process(y.data(), 1024, static_cast<float>(a.slot));
  fx.Process(y.data() + 1024, 1024, static_cast<float>(c.slot));
  double worst = 0.0;
  std::size_t at = 0;
  for (std::size_t i = 1024; i < 2048; ++i) {
    double want = 0.0;
    for (std::size_t k = 0; k < h2.size() && k <= i; ++k) want += static_cast<double>(h2[k]) * x[i - k];
    if (std::fabs(y[i] - want) > worst) {
      worst = std::fabs(y[i] - want);
      at = i;
    }
  }
  snd::test::Note("switch: worst %g at %zu", worst, at);
  SND_CHECK_MSG(worst < 1e-4, "after the switch at frame 1024: error %g at frame %zu (old IR tail)", worst, at);
}

// D-215 / convolution.hpp: "IrSlot is not ramped: a call uses the slot of its
// END value". A call longer than max_frames is split into sub-blocks by the
// effect framework (effects.cpp ProcessCore), which interpolates EVERY
// parameter at the sub-block boundaries, IrSlot included; fx_convolution.cpp
// ConvolutionProcess resolves p.e[kConvIrSlot] per sub-block. A ramp from slot
// A to slot C therefore plays the IRs of the slots in between (here slot B, a
// loud IR of another owner) for the first sub-blocks.
SND_TEST(IndepDefects, Defect_ConvolutionIrSlotRampsInsideLongCalls) {
  const std::uint32_t b = 64;
  std::vector<float> h1(200, 0.0f), hMid(200, 0.0f), h3(200, 0.0f);
  h1[0] = 0.5f;
  hMid[0] = 8.0f;  // the IR of the slot in between (must never be heard)
  h3[10] = 0.5f;
  PreparedIr a(h1, b), mid(hMid, b), c(h3, b);
  SND_REQUIRE(a.slot != 0u && c.slot == a.slot + 2u && mid.slot == a.slot + 1u);
  ConvFx fx(a.slot, b);
  SND_REQUIRE(EffectStateKind(fx.mem.get()) == EffectKind::Convolution);
  std::vector<float> x(1024);
  dsptest::Noise nz(4);
  for (float& v : x) v = nz.Next(0.5f);
  std::vector<float> y(x);
  // One call of 1024 frames (> max_frames 256), IrSlot ramps a -> c.
  const float s[1] = {static_cast<float>(a.slot)}, e[1] = {static_cast<float>(c.slot)};
  snd::ParamBlock pb{s, e, 1, 1, nullptr};
  float* p[1] = {y.data()};
  EffectProcess(fx.mem.get(), p, p, 1, 1024, &pb, nullptr);
  float peak = 0.0f;
  double worst = 0.0;
  for (std::size_t i = 0; i < 1024; ++i) {
    peak = std::max(peak, std::fabs(y[i]));
    const double want = i >= 10 ? 0.5 * x[i - 10] : 0.0;
    if (i >= 64) worst = std::max(worst, std::fabs(y[i] - want));  // the first partition may lag (defect above)
  }
  snd::test::Note("IrSlot %u -> %u in one 1024-frame call: output peak %g, worst error vs the end IR %g", a.slot,
                  c.slot, static_cast<double>(peak), worst);
  SND_CHECK_MSG(peak < 1.0f && worst < 1e-5, "the IR of the slot in between was used (peak %g)",
                static_cast<double>(peak));
}
