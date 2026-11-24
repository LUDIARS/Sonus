// Independent reproductions of unresolved D-218 / D-219 regressions.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/convolution.hpp"
#include "snd/dsp/time_stretch.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {
constexpr unsigned kRate = 48000;
struct Delta {
  dsptest::AlignedMem mem;
  const ConvolutionIr* ir;
  explicit Delta(float gain) {
    const float* taps[] = {&gain};
    const ConvolutionIrDesc desc{taps, 1, 1, kRate, 1.0f};
    const auto bytes = ConvolutionIrBytes(desc, kRate, 64);
    mem.Reset(bytes);
    ir = ConvolutionPrepareIr(mem.get(), bytes, desc, kRate, 64);
  }
};

struct Registrations {
  std::vector<unsigned> numbers;
  ~Registrations() {
    for (auto id : numbers) ConvolutionUnregisterIr(id);
  }
};
}

SND_TEST(IndepDefects, Defect_RegistryFindsSoleFreeEntryAtNumberWrap) {
  Delta delta(1.0f);
  Registrations owned;
  unsigned zeroEntry = 0;
  for (unsigned i = 0; i < 1024; ++i) {
    const unsigned id = ConvolutionRegisterIr(delta.ir);
    if (id == 0) break;
    if ((id & 1023u) == 0) zeroEntry = id;
    else owned.numbers.push_back(id);
  }
  SND_REQUIRE(zeroEntry != 0);
  // Keep every other entry busy. Each call must find entry zero again,
  // including the transition from number 16777215 back to number 1.
  bool crossed = false;
  for (unsigned generation = 0; generation < (1u << 14); ++generation) {
    const unsigned previous = zeroEntry;
    SND_REQUIRE(ConvolutionUnregisterIr(previous));
    zeroEntry = ConvolutionRegisterIr(delta.ir);
    SND_CHECK_MSG(zeroEntry != 0, "entry zero is vacant; last number %u", previous);
    if (zeroEntry == 0) break;
    if (zeroEntry < previous) {
      crossed = true;
      break;
    }
  }
  if (zeroEntry != 0) SND_CHECK(ConvolutionUnregisterIr(zeroEntry));
  SND_CHECK(crossed);
}

// The latest request takes effect on the first sample after the previous
// fade completes, independent of the internal processing block size.
SND_TEST(IndepDefects, Defect_PendingSwitchStartsAtFirstFreeSample) {
  Delta one(1), zero(0), negative(-1);
  Registrations owned;
  for (const auto* ir : {one.ir, zero.ir, negative.ir}) {
    auto id = ConvolutionRegisterIr(ir);
    SND_REQUIRE(id != 0);
    owned.numbers.push_back(id);
  }
  float params[kMaxEffectParams]{};
  EffectDefaultParams(EffectKind::Convolution, params);
  params[kConvIrSlot] = static_cast<float>(owned.numbers[0]);
  params[kConvPartitionFrames] = 64;
  snd::EffectDesc d{};
  d.effect_type = kEffectConvolution;
  d.channels = 1;
  d.max_frames = 13;
  d.param_count = kConvParamCount;
  d.initial_params = params;
  dsptest::AlignedMem mem(EffectStateBytes(d, kRate));
  SND_REQUIRE(EffectInit(mem.get(), mem.size(), d, kRate));
  std::vector<float> y(264, 1.0f);
  auto render = [&](unsigned start, unsigned length, unsigned index) {
    const float n = static_cast<float>(owned.numbers[index]);
    snd::ParamBlock p{&n, &n, 1, 1, nullptr};
    float* io[] = {y.data() + start};
    EffectProcess(mem.get(), io, io, 1, length, &p, nullptr);
  };
  render(0, 64, 0);
  render(64, 8, 1);   // fade 1 -> 0 spans [64, 128)
  render(72, 128, 2); // public call starts during that fade and ends at 200
  render(200, 64, 2);
  SND_CHECK_EQ(y[127], 0.0f);
  SND_CHECK_EQ(y[128], -1.0f / 64);
  SND_CHECK_EQ(y[191], -1.0f);
  SND_CHECK_EQ(y[192], -1.0f);
}

// Power-of-two scaling changes every nonzero candidate score by the same
// positive factor. It cannot change which source position wins (D-217).
SND_TEST(IndepDefects, Defect_QuietFiniteInputKeepsCorrelationChoice) {
  constexpr unsigned frames = 4096;
  std::vector<float> results[2];
  for (unsigned k = 0; k < 2; ++k) {
    const float scale = k ? 0x1p-60f : 1.0f;
    dsptest::AlignedMem mem(TimeStretchStateBytes(1, kRate));
    SND_REQUIRE(TimeStretchInit(mem.get(), mem.size(), 1, kRate));
    const unsigned n = TimeStretchInputFrames(mem.get(), frames, 2, 2);
    const unsigned pad = TimeStretchPadFrames(mem.get());
    std::vector<float> input(n + pad);
    dsptest::Noise noise(17);
    for (unsigned i = 0; i < n; ++i) input[pad + i] = scale * noise.Next(0.5f);
    results[k].resize(frames);
    float* in[] = {input.data()};
    float* out[] = {results[k].data()};
    SND_REQUIRE_EQ(TimeStretchProcess(mem.get(), in, n, out, frames, 2, 2), n);
    for (float& v : results[k]) v /= scale;
  }
  SND_CHECK(results[0] == results[1]);
}

SND_TEST(IndepDefects, Defect_FadeLastsFullPartitionAfterUnregister) {
  constexpr unsigned b = 64;
  // A nonzero tap in each of three partitions. An impulse after the switch
  // also checks that the old side uses real future input before fading out.
  std::vector<float> taps(2 * b + 3);
  taps[0] = 0.5f;
  taps[b + 1] = -0.25f;
  taps[2 * b + 2] = 0.125f;
  const float* channels[] = {taps.data()};
  const ConvolutionIrDesc irDesc{channels, 1, unsigned(taps.size()), kRate, 1.0f};
  for (unsigned latency : {0u, b}) {
    for (unsigned f : {0u, b / 2, b - 8, b - 1}) {
      dsptest::AlignedMem irMemory(ConvolutionIrBytes(irDesc, kRate, b));
      const auto* ir = ConvolutionPrepareIr(irMemory.get(), irMemory.size(), irDesc, kRate, b);
      const auto id = ConvolutionRegisterIr(ir);
      SND_REQUIRE(id != 0u);
      Registrations owned;
      owned.numbers.push_back(id);
      float params[kMaxEffectParams]{};
      EffectDefaultParams(EffectKind::Convolution, params);
      params[kConvPartitionFrames] = b;
      params[kConvZeroLatency] = latency == 0 ? 1.0f : 0.0f;
      params[kConvIrSlot] = static_cast<float>(id);
      snd::EffectDesc d{};
      d.effect_type = kEffectConvolution;
      d.channels = 1;
      d.max_frames = 37;
      d.param_count = kConvParamCount;
      d.initial_params = params;
      dsptest::AlignedMem mem(EffectStateBytes(d, kRate));
      SND_REQUIRE(EffectInit(mem.get(), mem.size(), d, kRate));
      std::vector<float> x(640);
      dsptest::Noise noise(42);
      for (float& v : x) v = noise.Next(0.5f);
      auto y = x;
      const unsigned start = 256 + f;
      float* before[] = {y.data()};
      EffectProcess(mem.get(), before, before, 1, start, nullptr, nullptr);
      SND_REQUIRE(ConvolutionUnregisterIr(id));
      owned.numbers.clear();
      std::memset(irMemory.get(), 0xff, irMemory.size());
      float* after[] = {y.data() + start};
      EffectProcess(mem.get(), after, after, 1, unsigned(y.size()) - start, nullptr, nullptr);
      double worst = 0;
      for (unsigned i = start; i < y.size(); ++i) {
        const auto t = i - latency;
        const double old = 0.5 * x[t] - 0.25 * x[t - b - 1] + 0.125 * x[t - 2 * b - 2];
        const double g = std::min(1.0, double(i - start + 1) / b);
        worst = std::max(worst, std::fabs(y[i] - (1.0 - g) * old));
      }
      // Three dyadic taps of magnitude <=1, two 128-point FFTs (7 stages).
      // Budget 2 float ulps per stage and one for interpolation.
      constexpr double tolerance = (2 * 2 * 7 + 1) * 0x1p-23;
      SND_CHECK_MSG(worst <= tolerance, "latency %u f=%u: full-fade error %.8g", latency, f, worst);
    }
  }
}
