// Convolution reverb (convolution.hpp, D-211..D-215): against a direct
// convolution in double, latency, block-size invariance, routing, blobs, the
// registry, tail, RT safety and cost.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/convolution.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr std::uint32_t kRate = 48000;
using Multi = std::vector<std::vector<float>>;  // [channel][frame]

// A prepared and registered IR (unregistered on destruction).
struct Ir {
  dsptest::AlignedMem mem;
  const ConvolutionIr* ir = nullptr;
  std::uint32_t slot = 0;
  Ir(const Multi& taps, std::uint32_t partition, std::uint32_t rate = kRate, float gain = 1.0f,
     std::uint32_t target = kRate) {
    std::vector<const float*> p;
    for (const auto& c : taps) p.push_back(c.data());
    const ConvolutionIrDesc d{p.data(), static_cast<std::uint32_t>(taps.size()),
                              static_cast<std::uint32_t>(taps[0].size()), rate, gain};
    const std::size_t b = ConvolutionIrBytes(d, target, partition);
    if (b == 0u) return;
    mem.Reset(b, dsptest::kDirty);
    ir = ConvolutionPrepareIr(mem.get(), mem.size(), d, target, partition);
    if (ir != nullptr) slot = ConvolutionRegisterIr(ir);
  }
  ~Ir() {
    if (slot != 0u) ConvolutionUnregisterIr(slot);
  }
  Ir(const Ir&) = delete;
  Ir& operator=(const Ir&) = delete;
};

struct Conv {
  dsptest::AlignedMem mem;
  snd::EffectDesc desc{};
  float params[kMaxEffectParams] = {};
  bool ok = false;
  Conv(std::uint32_t channels, std::uint32_t slot, std::uint32_t maxFrames, float partition = 0.0f, float zeroLatency = 1.0f,
       float maxIrMs = 2000.0f, float dry = 0.0f, float wet = 1.0f, float wetDb = 0.0f, std::uint32_t rate = kRate) {
    EffectDefaultParams(EffectKind::Convolution, params);
    params[kConvIrSlot] = static_cast<float>(slot);
    params[kConvDryMix] = dry;
    params[kConvWetMix] = wet;
    params[kConvWetGainDb] = wetDb;
    params[kConvPartitionFrames] = partition;
    params[kConvZeroLatency] = zeroLatency;
    params[kConvMaxIrMs] = maxIrMs;
    desc.effect_type = kEffectConvolution;
    desc.channels = channels;
    desc.max_frames = maxFrames;
    desc.param_count = kConvParamCount;
    desc.initial_params = params;
    const std::uint32_t bytes = EffectStateBytes(desc, rate);
    mem.Reset(bytes, dsptest::kDirty);
    ok = bytes > 0u && EffectInit(mem.get(), mem.size(), desc, rate);
  }
  void* st() const { return mem.get(); }
};

// Runs `in` through the instance in calls of the given sizes (cycled).
Multi Run(const Conv& fx, const Multi& in, const std::vector<std::uint32_t>& sizes, std::uint32_t outChannels = 0) {
  const std::uint32_t ch = outChannels != 0u ? outChannels : static_cast<std::uint32_t>(in.size());
  const std::uint32_t n = static_cast<std::uint32_t>(in[0].size());
  Multi out(ch, std::vector<float>(n, 0.0f));
  std::uint32_t done = 0, k = 0;
  while (done < n) {
    std::uint32_t len = sizes[k++ % sizes.size()];
    if (len > n - done) len = n - done;
    std::vector<std::vector<float>> buf(ch, std::vector<float>(len));
    float* p[8];
    for (std::uint32_t c = 0; c < ch; ++c) {
      std::memcpy(buf[c].data(), in[c].data() + done, sizeof(float) * len);
      p[c] = buf[c].data();
    }
    {
      SND_RT_SCOPE();
      EffectProcess(fx.st(), p, p, ch, len, nullptr, nullptr);  // in place
    }
    for (std::uint32_t c = 0; c < ch; ++c) std::memcpy(out[c].data() + done, buf[c].data(), sizeof(float) * len);
    done += len;
  }
  return out;
}

// y = x * h (double), first n samples, delayed by `delay`.
std::vector<double> Direct(const std::vector<float>& x, const std::vector<float>& h, std::uint32_t delay = 0) {
  const std::size_t n = x.size();
  std::vector<double> y(n, 0.0);
  for (std::size_t i = delay; i < n; ++i) {
    const std::size_t t = i - delay;
    double acc = 0.0;
    const std::size_t kmax = t + 1 < h.size() ? t + 1 : h.size();
    for (std::size_t k = 0; k < kmax; ++k) acc += static_cast<double>(h[k]) * x[t - k];
    y[i] = acc;
  }
  return y;
}

double MaxErr(const std::vector<float>& got, const std::vector<double>& want) {
  double e = 0.0;
  for (std::size_t i = 0; i < got.size(); ++i) e = std::max(e, std::fabs(got[i] - want[i]));
  return e;
}

std::vector<float> NoiseVec(std::uint32_t n, std::uint32_t seed, float amp = 0.5f) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (float& x : v) x = nz.Next(amp);
  return v;
}

// Exponentially decaying noise (a reverb-like IR), unit-ish energy.
std::vector<float> DecayIr(std::uint32_t n, std::uint32_t seed, double tau) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (std::uint32_t i = 0; i < n; ++i) v[i] = static_cast<float>(nz.Next(1.0f) * std::exp(-static_cast<double>(i) / tau) * 0.3);
  return v;
}

}  // namespace

SND_TEST(Convolution, RegistryEntry) {
  const EffectInfo* info = FindEffect(kEffectConvolution);
  SND_REQUIRE(info != nullptr);
  SND_CHECK(info->kind == EffectKind::Convolution);
  SND_CHECK_EQ(info->paramCount, static_cast<std::uint32_t>(kConvParamCount));
  SND_CHECK((info->flags & kEffectHasLatency) != 0u);
  SND_CHECK((info->params[kConvMaxIrMs].flags & kParamInitOnly) != 0u);
  SND_CHECK((info->params[kConvIrSlot].flags & kParamInteger) != 0u);
  SND_CHECK_EQ(info->params[kConvIrSlot].maxValue, static_cast<float>(kConvolutionMaxIrNumber));
}

// An impulse IR gives the input back: exactly with the direct head, and delayed by
// exactly B (FFT rounding only) with ZeroLatency 0. A delayed impulse delays.
SND_TEST(Convolution, ImpulseIrIsIdentityAndLatencyIsAsSpecified) {
  const std::vector<float> x = NoiseVec(6000, 3);
  for (std::uint32_t b : {64u, 256u}) {
    for (std::uint32_t d : {0u, 5u, b - 1u, b, b + 3u, 3u * b + 17u}) {
      std::vector<float> h(d + 1u, 0.0f);
      h[d] = 1.0f;
      Ir ir({h}, b);
      SND_REQUIRE(ir.slot != 0u);
      for (float zl : {1.0f, 0.0f}) {
        Conv fx(1, ir.slot, 256, static_cast<float>(b), zl);
        SND_REQUIRE(fx.ok);
        const std::uint32_t lat = EffectLatencyFrames(fx.st());
        SND_CHECK_EQ(lat, zl != 0.0f ? 0u : b);
        const Multi y = Run(fx, {x}, {256});
        double e = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
          const std::size_t t = i >= d + lat ? i - d - lat : static_cast<std::size_t>(-1);
          const double want = t != static_cast<std::size_t>(-1) ? x[t] : 0.0;
          e = std::max(e, std::fabs(y[0][i] - want));
        }
        if (zl != 0.0f && d < b) {
          SND_CHECK_MSG(e == 0.0, "B %u delay %u: direct head not exact (%g)", b, d, e);
        } else {
          SND_CHECK_MSG(e < 2e-6, "B %u delay %u zl %d: err %g", b, d, static_cast<int>(zl), e);
        }
      }
    }
  }
}

// Short (< B) and long (many partitions) IRs vs the direct convolution, both modes.
SND_TEST(Convolution, MatchesDirectConvolution) {
  const std::vector<float> x = NoiseVec(14000, 7);
  struct Case {
    std::uint32_t taps, b;
  };
  for (const Case cs : {Case{40, 64}, Case{100, 256}, Case{3000, 64}, Case{3000, 256}, Case{9000, 512}}) {
    const std::vector<float> h = DecayIr(cs.taps, cs.taps + 1u, cs.taps / 4.0);
    Ir ir({h}, cs.b);
    SND_REQUIRE(ir.slot != 0u);
    for (float zl : {1.0f, 0.0f}) {
      Conv fx(1, ir.slot, 256, static_cast<float>(cs.b), zl);
      SND_REQUIRE(fx.ok);
      const Multi y = Run(fx, {x}, {256});
      const std::vector<double> want = Direct(x, h, EffectLatencyFrames(fx.st()));
      double peak = 0.0;
      for (double v : want) peak = std::max(peak, std::fabs(v));
      const double e = MaxErr(y[0], want);
      SND_CHECK_MSG(e < 2e-5 * (1.0 + peak), "taps %u B %u zl %d: err %g (peak %g)", cs.taps, cs.b, static_cast<int>(zl), e,
                    peak);
      SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), cs.taps);
    }
  }
}

// Bit-identical however the input is split into calls (constant parameters), and
// the same for a different max_frames with the same partition.
SND_TEST(Convolution, BlockSizeDoesNotChangeTheResult) {
  const std::vector<float> h = DecayIr(2500, 11, 600.0);
  const Multi x = {NoiseVec(9000, 13), NoiseVec(9000, 14)};
  Ir ir({h}, 128);
  SND_REQUIRE(ir.slot != 0u);
  for (float zl : {1.0f, 0.0f}) {
    Conv a(2, ir.slot, 256, 128.0f, zl, 2000.0f, 0.3f, 0.8f);
    const Multi ya = Run(a, x, {256});
    for (const std::vector<std::uint32_t>& sizes :
         {std::vector<std::uint32_t>{1}, std::vector<std::uint32_t>{64}, std::vector<std::uint32_t>{1000},
          std::vector<std::uint32_t>{7, 300, 129, 1, 555, 64}}) {
      for (std::uint32_t maxFrames : {256u, 64u, 1024u}) {
        Conv b(2, ir.slot, maxFrames, 128.0f, zl, 2000.0f, 0.3f, 0.8f);
        const Multi yb = Run(b, x, sizes);
        SND_CHECK_MSG(ya == yb, "zl %d sizes[0] %u max_frames %u: differs", static_cast<int>(zl), sizes[0], maxFrames);
      }
    }
  }
  // Auto partition from max_frames (256 vs 512): different B, both match the reference.
  for (std::uint32_t maxFrames : {256u, 512u}) {
    Ir irb({h}, maxFrames);
    Conv c(1, irb.slot, maxFrames);
    SND_CHECK_EQ(ConvolutionStatePartition(c.st()), maxFrames);
    snd::EffectDesc d = c.desc;
    SND_CHECK_EQ(ConvolutionPartitionFrames(d), maxFrames);
    const Multi y = Run(c, {x[0]}, {maxFrames});
    SND_CHECK(MaxErr(y[0], Direct(x[0], h)) < 2e-5);
  }
}

// Routing: per channel (stereo IR), matrix (true stereo, LL LR RL RR), modulo (mono IR on 3 ch).
SND_TEST(Convolution, ChannelRouting) {
  const Multi x = {NoiseVec(5000, 21), NoiseVec(5000, 22), NoiseVec(5000, 23)};
  const Multi irs = {DecayIr(700, 1, 200.0), DecayIr(700, 2, 150.0), DecayIr(700, 3, 100.0), DecayIr(700, 4, 50.0)};
  {
    Ir ir({irs[0], irs[1]}, 128);
    Conv fx(2, ir.slot, 256, 128.0f);
    const Multi y = Run(fx, {x[0], x[1]}, {256});
    SND_CHECK(MaxErr(y[0], Direct(x[0], irs[0])) < 2e-5);
    SND_CHECK(MaxErr(y[1], Direct(x[1], irs[1])) < 2e-5);
  }
  for (float zl : {1.0f, 0.0f}) {
    Ir ir({irs[0], irs[1], irs[2], irs[3]}, 128);
    Conv fx(2, ir.slot, 256, 128.0f, zl);
    const Multi y = Run(fx, {x[0], x[1]}, {256});
    const std::uint32_t lat = EffectLatencyFrames(fx.st());
    for (std::uint32_t o = 0; o < 2; ++o) {
      std::vector<double> want = Direct(x[0], irs[0 * 2 + o], lat);
      const std::vector<double> w2 = Direct(x[1], irs[1 * 2 + o], lat);
      for (std::size_t i = 0; i < want.size(); ++i) want[i] += w2[i];
      SND_CHECK_MSG(MaxErr(y[o], want) < 3e-5, "true stereo out %u zl %d: %g", o, static_cast<int>(zl), MaxErr(y[o], want));
    }
  }
  {
    Ir ir({irs[2]}, 128);
    Conv fx(3, ir.slot, 256, 128.0f);
    const Multi y = Run(fx, x, {256});
    for (std::uint32_t c = 0; c < 3; ++c) SND_CHECK(MaxErr(y[c], Direct(x[c], irs[2])) < 2e-5);
    // A call with fewer channels: the missing inputs are silence, nothing breaks.
    const Multi y2 = Run(fx, {x[0]}, {256}, 1);
    SND_CHECK(y2[0].size() == x[0].size());
  }
}

// out = DryMix * x + WetMix * 10^(WetGainDb / 20) * (x * h); with the latency mode
// the dry part is delayed by B too.
SND_TEST(Convolution, DryWetGains) {
  const std::vector<float> x = NoiseVec(4000, 31);
  const std::vector<float> h = DecayIr(500, 32, 100.0);
  Ir ir({h}, 64);
  for (float zl : {1.0f, 0.0f}) {
    Conv fx(1, ir.slot, 256, 64.0f, zl, 2000.0f, 0.5f, 0.25f, 6.0f);
    const Multi y = Run(fx, {x}, {256});
    const std::uint32_t lat = EffectLatencyFrames(fx.st());
    std::vector<double> want = Direct(x, h, lat);
    const double g = 0.25 * std::pow(10.0, 6.0 / 20.0);
    for (std::size_t i = 0; i < want.size(); ++i) want[i] = want[i] * g + (i >= lat ? 0.5 * x[i - lat] : 0.0);
    SND_CHECK_MSG(MaxErr(y[0], want) < 2e-5, "zl %d: %g", static_cast<int>(zl), MaxErr(y[0], want));
  }
  // Ramped dry gain: per frame linear between the block's start and end values.
  Conv fx(1, 0, 256, 64.0f, 1.0f, 100.0f);
  float s[kConvParamCount], e[kConvParamCount];
  std::memcpy(s, fx.params, sizeof(s));
  std::memcpy(e, fx.params, sizeof(e));
  s[kConvDryMix] = 0.0f;
  e[kConvDryMix] = 1.0f;
  snd::ParamBlock pb{s, e, kConvParamCount, 0xFFFFFFFFu, nullptr};
  std::vector<float> one(256, 1.0f);
  float* p[1] = {one.data()};
  {
    SND_RT_SCOPE();
    EffectProcess(fx.st(), p, p, 1, 256, &pb, nullptr);
  }
  double err = 0.0;
  for (std::uint32_t i = 0; i < 256; ++i) err = std::max(err, std::fabs(one[i] - i / 256.0));
  SND_CHECK(err < 1e-6);
}

// IR at another rate is resampled when prepared: a 1 kHz tone burst at 44.1 kHz
// comes out as the same burst at 48 kHz (impulse response of the instance).
SND_TEST(Convolution, IrIsResampledToTheTargetRate) {
  for (std::uint32_t src : {44100u, 24000u, 96000u}) {
    const std::uint32_t frames = src / 10u;  // 100 ms
    std::vector<float> h(frames);
    auto shape = [](double t) {  // seconds -> value (Hann-windowed 1 kHz tone over 100 ms)
      if (t < 0.0 || t > 0.1) return 0.0;
      return 0.5 * (1.0 - std::cos(2.0 * dsptest::kPi * t / 0.1)) * std::sin(2.0 * dsptest::kPi * 1000.0 * t);
    };
    for (std::uint32_t i = 0; i < frames; ++i) h[i] = static_cast<float>(shape(static_cast<double>(i) / src));
    Ir ir({h}, 256, src);
    SND_REQUIRE(ir.slot != 0u);
    SND_CHECK_EQ(ConvolutionIrFrames(ir.ir), ConvolutionIrTargetFrames(frames, src, kRate));
    SND_CHECK_EQ(ConvolutionIrSampleRate(ir.ir), kRate);
    Conv fx(1, ir.slot, 256);
    std::vector<float> x(6000, 0.0f);
    x[0] = 1.0f;
    const Multi y = Run(fx, {x}, {256});
    double e = 0.0;
    for (std::uint32_t i = 200; i < 4600; ++i) e = std::max(e, std::fabs(y[0][i] - shape(static_cast<double>(i) / kRate)));
    SND_CHECK_MSG(e < 2e-3, "IR at %u Hz: max err %g against the analytic IR at 48 kHz", src, e);
  }
}

// IR blob: write, parse, prepare == prepare from arrays; malformed blobs are rejected.
SND_TEST(Convolution, BlobRoundTripAndValidation) {
  const Multi taps = {DecayIr(1000, 41, 300.0), DecayIr(1000, 42, 300.0)};
  const float* p[2] = {taps[0].data(), taps[1].data()};
  std::vector<unsigned char> blob(kConvolutionBlobHeaderBytes + 4u * 2u * 1000u + 3u);
  unsigned char* at = blob.data() + 3;  // unaligned
  const std::size_t n = ConvolutionWriteIrBlob(at, blob.size() - 3u, p, 2, 1000, kRate, 0.5f);
  SND_REQUIRE_EQ(n, blob.size() - 3u);
  ConvolutionBlobInfo bi{};
  SND_REQUIRE(ConvolutionParseIrBlob(at, n, &bi));
  SND_CHECK(bi.channels == 2u && bi.frames == 1000u && bi.sampleRate == kRate && bi.gain == 0.5f);
  const std::size_t bytes = ConvolutionIrBytesFromBlob(at, n, kRate, 128);
  const ConvolutionIrDesc d{p, 2, 1000, kRate, 0.5f};
  SND_REQUIRE_EQ(bytes, ConvolutionIrBytes(d, kRate, 128));
  dsptest::AlignedMem m1(bytes, 0x5A), m2(bytes, 0xA5);
  SND_REQUIRE(ConvolutionPrepareIrFromBlob(m1.get(), bytes, at, n, kRate, 128) != nullptr);
  SND_REQUIRE(ConvolutionPrepareIr(m2.get(), bytes, d, kRate, 128) != nullptr);
  SND_CHECK(std::memcmp(m1.get(), m2.get(), bytes) == 0);
  // Malformed.
  SND_CHECK(!ConvolutionParseIrBlob(at, n - 1u, nullptr));
  SND_CHECK(!ConvolutionParseIrBlob(at, 16, nullptr));
  SND_CHECK(!ConvolutionParseIrBlob(nullptr, n, nullptr));
  for (std::uint32_t field = 0; field < 8; ++field) {
    std::vector<unsigned char> bad(at, at + n);
    std::uint32_t v;
    std::memcpy(&v, bad.data() + 4 * field, 4);
    const std::uint32_t badValues[8] = {0x52494354u, 2u, 17u, 0u, 0u, 0x7FC00000u, 1u, 1u};
    if (field == 4) v = kConvolutionMaxRate + 1u; else v = badValues[field];
    std::memcpy(bad.data() + 4 * field, &v, 4);
    SND_CHECK_MSG(!ConvolutionParseIrBlob(bad.data(), bad.size(), nullptr), "header field %u not checked", field);
    SND_CHECK_EQ(ConvolutionIrBytesFromBlob(bad.data(), bad.size(), kRate, 128), 0u);
  }
  // Invalid preparation arguments.
  SND_CHECK_EQ(ConvolutionIrBytes(d, kRate, 100), 0u);   // not a power of two
  SND_CHECK_EQ(ConvolutionIrBytes(d, kRate, 32), 0u);    // below 64
  SND_CHECK_EQ(ConvolutionIrBytes(d, kRate, 8192), 0u);  // above 4096
  SND_CHECK_EQ(ConvolutionIrBytes(d, 0, 128), 0u);
  SND_CHECK(ConvolutionPrepareIr(m2.get(), bytes - 64u, d, kRate, 128) == nullptr);
}

SND_TEST(Convolution, Registry) {
  const std::vector<float> h = {1.0f, 0.5f};
  Ir a({h}, 64), b({h}, 64);
  SND_REQUIRE(a.slot != 0u && b.slot != 0u && a.slot != b.slot);
  SND_CHECK(ConvolutionRegisteredIr(a.slot) == a.ir);
  SND_CHECK_EQ(ConvolutionRegisterIr(nullptr), 0u);
  int garbage[32] = {};
  SND_CHECK_EQ(ConvolutionRegisterIr(reinterpret_cast<const ConvolutionIr*>(garbage)), 0u);
  SND_CHECK(ConvolutionRegisteredIr(0) == nullptr);
  SND_CHECK(ConvolutionRegisteredIr(kConvolutionMaxIrSlots + 1u) == nullptr);
  SND_CHECK(!ConvolutionUnregisterIr(0));
  // Numbers carry a generation (D-219): a freed number stays dead when its entry is
  // reused, only the registering code's number unregisters, nothing assumes that the
  // registry is otherwise empty.
  SND_CHECK(a.slot <= kConvolutionMaxIrNumber && b.slot <= kConvolutionMaxIrNumber);
  SND_CHECK(!ConvolutionUnregisterIr(a.slot + kConvolutionMaxIrSlots));  // same entry, other generation
  SND_CHECK(ConvolutionRegisteredIr(a.slot) == a.ir);
  const std::uint32_t c = ConvolutionRegisterIr(b.ir);
  SND_REQUIRE(c != 0u);
  SND_CHECK(ConvolutionUnregisterIr(c));
  SND_CHECK(!ConvolutionUnregisterIr(c));
  SND_CHECK(ConvolutionRegisteredIr(c) == nullptr);
  std::vector<std::uint32_t> taken;
  for (;;) {  // fill what is free; numbers are unique and none resolves to the old c
    const std::uint32_t n = ConvolutionRegisterIr(a.ir);
    if (n == 0u) break;
    taken.push_back(n);
  }
  SND_CHECK(taken.size() + 2u <= static_cast<std::size_t>(kConvolutionMaxIrSlots));
  SND_CHECK(ConvolutionRegisteredIr(c) == nullptr);
  bool unique = true;
  for (std::size_t i = 0; i < taken.size(); ++i) unique &= taken[i] != c && taken[i] != a.slot && taken[i] != b.slot;
  SND_CHECK(unique);
  for (std::uint32_t n : taken) SND_CHECK(ConvolutionUnregisterIr(n));
  SND_CHECK(ConvolutionRegisteredIr(a.slot) == a.ir && ConvolutionRegisteredIr(b.slot) == b.ir);
}

// An IR made for another partition or rate is not used (wet silent, dry passes).
// Switching IrSlot between calls switches the IR; MaxIrMs cuts long IRs.
SND_TEST(Convolution, IrSelectionAndCapacity) {
  const std::vector<float> x = NoiseVec(3000, 51);
  const std::vector<float> h = DecayIr(800, 52, 200.0);
  {
    Ir wrongB({h}, 128), wrongRate({h}, 256, kRate, 1.0f, 44100);
    for (std::uint32_t slot : {wrongB.slot, wrongRate.slot, 999u}) {
      Conv fx(1, slot, 256, 256.0f, 1.0f, 2000.0f, 1.0f, 1.0f);
      const Multi y = Run(fx, {x}, {256});
      SND_CHECK(y[0] == x);
      SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), 0u);
    }
  }
  {
    Ir i1({h}, 64), i2({DecayIr(300, 53, 50.0)}, 64);
    Conv fx(1, i1.slot, 256, 64.0f);
    std::vector<float> a(x.begin(), x.begin() + 1024), b2(x.begin() + 1024, x.end());
    Run(fx, {a}, {256});
    fx.params[kConvIrSlot] = static_cast<float>(i2.slot);
    snd::ParamBlock pb{fx.params, fx.params, kConvParamCount, 1u, nullptr};
    std::vector<float> buf(256, 0.0f);
    float* p[1] = {buf.data()};
    EffectProcess(fx.st(), p, p, 1, 256, &pb, nullptr);
    SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), 300u);
    SND_CHECK_EQ(static_cast<std::uint32_t>(EffectParamValue(fx.st(), kConvIrSlot)), i2.slot);
  }
  {
    // 10 ms of capacity at 48 kHz = 480 frames -> 8 partitions of 64 = 512 frames.
    const std::vector<float> longH = DecayIr(4000, 54, 2000.0);
    Ir ir({longH}, 64);
    Conv fx(1, ir.slot, 256, 64.0f, 1.0f, 10.0f);
    SND_CHECK_EQ(ConvolutionStateCapacityFrames(fx.st()), 512u + 64u);  // FDL + direct head (D-218)
    const Multi y = Run(fx, {x}, {256});
    SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), 512u + 64u);  // head + 8 FDL partitions
    std::vector<float> cut(longH.begin(), longH.begin() + 576);
    SND_CHECK(MaxErr(y[0], Direct(x, cut)) < 2e-5);
  }
}

// Tail: done after the input is silent for capacity + 2B (+B) frames, then exact
// zeros at no cost; the next burst equals the direct convolution again.
SND_TEST(Convolution, TailAndWakeUp) {
  const std::vector<float> h = DecayIr(2000, 61, 400.0);
  Ir ir({h}, 128);
  for (float zl : {1.0f, 0.0f}) {
    Conv fx(2, ir.slot, 256, 128.0f, zl, 100.0f);  // capacity 4800 -> 38 partitions
    SND_REQUIRE(fx.ok);
    SND_CHECK(EffectIsTailDone(fx.st()));
    const std::uint32_t cap = ConvolutionStateCapacityFrames(fx.st()) - (zl != 0.0f ? 128u : 0u);  // FDL part
    const std::uint32_t tail = cap + 2u * 128u + (zl != 0.0f ? 0u : 128u);
    std::vector<float> burst = NoiseVec(1000, 62);
    burst.resize(1000 + tail + 2000, 0.0f);
    const Multi y = Run(fx, {burst, burst}, {100});
    SND_CHECK(EffectIsTailDone(fx.st()));
    for (std::size_t i = 1000 + tail; i < burst.size(); ++i) SND_CHECK(y[0][i] == 0.0f && y[1][i] == 0.0f);
    // Silence keeps it exactly silent; a new burst is convolved normally.
    std::vector<float> again = NoiseVec(3000, 63);
    const Multi y2 = Run(fx, {again, again}, {256});
    SND_CHECK(!EffectIsTailDone(fx.st()));
    SND_CHECK(MaxErr(y2[0], Direct(again, h, EffectLatencyFrames(fx.st()))) < 2e-5);
    // Not done one frame early.
    Conv f2(1, ir.slot, 256, 128.0f, zl, 100.0f);
    std::vector<float> one(1000 + tail - 1, 0.0f);
    one[999] = 0.5f;
    Run(f2, {one}, {256});
    SND_CHECK(!EffectIsTailDone(f2.st()));
    std::vector<float> z(1, 0.0f);
    Run(f2, {z}, {1});
    SND_CHECK(EffectIsTailDone(f2.st()));
  }
}

// Batch processing equals single processing.
SND_TEST(Convolution, BatchEqualsSingle) {
  const std::vector<float> h = DecayIr(900, 71, 300.0);
  Ir ir({h}, 64);
  Conv a(1, ir.slot, 256, 64.0f), b(1, ir.slot, 256, 64.0f), c(1, ir.slot, 256, 64.0f, 0.0f);
  std::vector<float> x = NoiseVec(256, 72), xa = x, xb = x, xc = x;
  float* pa[1] = {xa.data()};
  float* pb[1] = {xb.data()};
  float* pc[1] = {xc.data()};
  for (int k = 0; k < 6; ++k) {
    EffectProcess(a.st(), pa, pa, 1, 256, nullptr, nullptr);
    const EffectBatchItem items[2] = {{b.st(), pb, pb, 1, 256, nullptr, nullptr}, {c.st(), pc, pc, 1, 256, nullptr, nullptr}};
    {
      SND_RT_SCOPE();
      EffectProcessBatch(EffectKind::Convolution, items, 2);
    }
    SND_CHECK(xa == xb);
  }
}

// CPU per block (Release numbers are the meaningful ones): 2 s stereo IR at 48 kHz.
SND_TEST(Convolution, Cost) {
  const std::uint32_t frames = 2u * kRate;
  const Multi taps = {DecayIr(frames, 81, kRate * 0.4), DecayIr(frames, 82, kRate * 0.4)};
  struct Case {
    std::uint32_t block, b;
    float zl;
  };
  for (const Case cs : {Case{256, 256, 1.0f}, Case{256, 256, 0.0f}, Case{512, 512, 1.0f}, Case{512, 512, 0.0f},
                        Case{256, 512, 1.0f}}) {
    Ir ir(taps, cs.b);
    SND_REQUIRE(ir.slot != 0u);
    Conv fx(2, ir.slot, cs.block, static_cast<float>(cs.b), cs.zl, 2000.0f);
    SND_REQUIRE(fx.ok);
    std::vector<float> l = NoiseVec(cs.block, 83), r = NoiseVec(cs.block, 84);
    float* p[2] = {l.data(), r.data()};
    // Warm up until the delay line is full (cost is then constant per partition).
    for (std::uint32_t k = 0; k < frames / cs.block + 4u; ++k) EffectProcess(fx.st(), p, p, 2, cs.block, nullptr, nullptr);
#ifdef NDEBUG
    const int blocks = 400;
#else
    const int blocks = 40;
#endif
    double worst = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < blocks; ++k) {
      for (std::uint32_t i = 0; i < cs.block; ++i) l[i] = r[i] = 0.1f;
      const auto a = std::chrono::steady_clock::now();
      EffectProcess(fx.st(), p, p, 2, cs.block, nullptr, nullptr);
      worst = std::max(worst, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    snd::test::Note("convolution 2 s stereo IR, block %u, B %u, %s: %.1f us per block (worst %.1f), budget %.0f us, state %u bytes",
                    cs.block, cs.b, cs.zl != 0.0f ? "zero latency" : "latency B", us, worst, 1e6 * cs.block / kRate,
                    EffectStateBytes(fx.desc, kRate));
  }
}

namespace {
// y = sum h[k] x[i - k] for frame i (double).
double ConvAt(const std::vector<float>& x, const std::vector<float>& h, std::size_t i) {
  double acc = 0.0;
  for (std::size_t k = 0; k < h.size() && k <= i; ++k) acc += static_cast<double>(h[k]) * x[i - k];
  return acc;
}
}  // namespace

// D-218: switching the IR cross-fades linearly from the switch to the end of the
// partition period (B frames when the switch is on a period boundary), old side =
// the old IR on the full history, new side = the new IR on the full history; after
// the fade the new IR alone. The same with the switch inside a period, and
// bit-identical however the calls are split around the same switch frame.
SND_TEST(Convolution, IrSwitchCrossFades) {
  const std::uint32_t b = 64;
  const std::vector<float> h1 = DecayIr(600, 91, 150.0), h2 = DecayIr(600, 92, 150.0);
  Ir i1({h1}, b), i2({h2}, b);
  SND_REQUIRE(i1.slot != 0u && i2.slot != 0u);
  const std::vector<float> x = NoiseVec(3000, 93);
  for (std::uint32_t sw : {1024u, 1000u}) {  // on / inside a period (1000 % 64 = 40)
    for (float zl : {1.0f, 0.0f}) {
      std::vector<std::vector<float>> outs;
      for (const std::vector<std::uint32_t>& sizes : {std::vector<std::uint32_t>{sw, 3000u - sw},
                                                      std::vector<std::uint32_t>{sw / 2u, sw - sw / 2u, 7u, 3000u}}) {
        Conv fx(1, i1.slot, 256, static_cast<float>(b), zl, 100.0f);
        std::vector<float> y(x);
        std::uint32_t done = 0;
        for (std::uint32_t n : sizes) {
          if (n > 3000u - done) n = 3000u - done;
          fx.params[kConvIrSlot] = static_cast<float>(done >= sw ? i2.slot : i1.slot);
          snd::ParamBlock pb{fx.params, fx.params, kConvParamCount, 1u, nullptr};
          float* p[1] = {y.data() + done};
          SND_RT_SCOPE();
          EffectProcess(fx.st(), p, p, 1, n, &pb, nullptr);
          done += n;
          if (done >= 3000u) break;
        }
        outs.push_back(y);
      }
      SND_CHECK_MSG(outs[0] == outs[1], "switch at %u zl %d: depends on the call split", sw, static_cast<int>(zl));
      const std::uint32_t lat = zl != 0.0f ? 0u : b;
      const std::uint32_t f0 = sw % b, end = sw - f0 + b;
      double err = 0.0;
      for (std::size_t i = lat; i < 3000u; ++i) {
        const double o = ConvAt(x, h1, i - lat), n = ConvAt(x, h2, i - lat);
        double want = i < sw ? o : n;
        if (i >= sw && i < end) {
          const double g = static_cast<double>(i - sw + 1u) / static_cast<double>(b - f0);
          want = o + g * (n - o);
        }
        err = std::max(err, std::fabs(outs[0][i] - want));
      }
      SND_CHECK_MSG(err < 2e-5, "switch at %u zl %d: err %g", sw, static_cast<int>(zl), err);
    }
  }
}

// D-218 / D-219: after the IR is unregistered the instance fades to silence without
// reading the old IR (its memory is overwritten with NaN right after the unregister).
SND_TEST(Convolution, UnregisterFadesOutWithoutReadingTheIr) {
  const std::uint32_t b = 64;
  const std::vector<float> h = DecayIr(500, 95, 120.0);
  auto ir = std::make_unique<Ir>(std::vector<std::vector<float>>{h}, b);
  const std::uint32_t num = ir->slot;
  Conv fx(1, num, 256, static_cast<float>(b));
  const std::vector<float> x = NoiseVec(1536, 96);
  std::vector<float> y(x);
  float* p[1] = {y.data()};
  EffectProcess(fx.st(), p, p, 1, 1024, nullptr, nullptr);
  SND_REQUIRE(ConvolutionUnregisterIr(num));
  ir->slot = 0;
  std::memset(ir->mem.get(), 0xFF, ir->mem.size());  // freed and reused
  float* q[1] = {y.data() + 1024};
  {
    SND_RT_SCOPE();
    EffectProcess(fx.st(), q, q, 1, 512, nullptr, nullptr);
  }
  double err = 0.0;
  for (std::size_t i = 1024; i < 1536; ++i) {
    const double g = i < 1088 ? static_cast<double>(i - 1024 + 1) / 64.0 : 1.0;
    const double want = (1.0 - g) * ConvAt(x, h, i);
    err = std::max(err, std::fabs(y[i] - want));
  }
  SND_CHECK_MSG(err < 2e-5, "fade out after unregister: err %g", err);
  SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), 0u);
}

// D-219: IRs with NaN / Inf samples are refused (arrays and blobs).
SND_TEST(Convolution, NonFiniteIrIsRefused) {
  for (float bad : {std::nanf(""), INFINITY, -INFINITY}) {
    std::vector<float> h = DecayIr(300, 97, 80.0);
    h[123] = bad;
    const float* p[1] = {h.data()};
    const ConvolutionIrDesc d{p, 1, 300, kRate, 1.0f};
    const std::size_t bytes = ConvolutionIrBytes(d, kRate, 64);
    SND_REQUIRE(bytes > 0u);
    dsptest::AlignedMem m(bytes);
    SND_CHECK(ConvolutionPrepareIr(m.get(), bytes, d, kRate, 64) == nullptr);
    std::vector<unsigned char> blob(kConvolutionBlobHeaderBytes + 4u * 300u);
    SND_REQUIRE(ConvolutionWriteIrBlob(blob.data(), blob.size(), p, 1, 300, kRate, 1.0f) == blob.size());
    SND_CHECK(!ConvolutionParseIrBlob(blob.data(), blob.size(), nullptr));
    SND_CHECK(ConvolutionPrepareIrFromBlob(m.get(), bytes, blob.data(), blob.size(), kRate, 64) == nullptr);
  }
}

// D-218: IrSlot never takes an intermediate value inside a call longer than
// max_frames (the end value of the whole call is used for every sub-block).
SND_TEST(Convolution, LongCallUsesTheEndSlot) {
  const std::uint32_t b = 64;
  std::vector<float> h1(100, 0.0f), h3(100, 0.0f);
  h1[0] = 0.5f;
  h3[10] = 0.5f;
  Ir a({h1}, b);
  std::vector<std::unique_ptr<Ir>> between;
  for (int k = 0; k < 6; ++k) between.push_back(std::make_unique<Ir>(std::vector<std::vector<float>>{std::vector<float>(100, 9.0f)}, b));
  Ir c({h3}, b);
  Conv fx(1, a.slot, 256, static_cast<float>(b));
  const std::vector<float> x = NoiseVec(2048, 98);
  std::vector<float> y(x);
  const float s0[1] = {static_cast<float>(a.slot)}, s1[1] = {static_cast<float>(c.slot)};
  snd::ParamBlock pb{s0, s1, 1, 1, nullptr};
  float* p[1] = {y.data()};
  EffectProcess(fx.st(), p, p, 1, 2048, &pb, nullptr);
  double err = 0.0;
  for (std::size_t i = 64; i < 2048; ++i) err = std::max(err, std::fabs(y[i] - (i >= 10 ? 0.5 * x[i - 10] : 0.0)));
  SND_CHECK_MSG(err < 1e-5, "an intermediate IR was used: err %g", err);
}
