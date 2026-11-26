// Independent tests of the convolution reverb snd.fx.convolution
// (convolution.hpp, CONTRACTS.md D-211..D-216), written from the specification.
// Expectations are computed here (direct convolution in double, own routing,
// own ramps); the implementation is never used to compute an expected value.
//   - effect info, parameter clamping, partition rule, state size (D-214), guards
//   - impulse IRs at every interesting delay, both latency modes, dry latency
//   - IR lengths around the partition boundaries (B-1, B, B+1, 2B-1, 2B, ...) and
//     a 2 s IR against a direct convolution, block sizes 1 / B-1 / B / B+1 / prime /
//     variable / > max_frames
//   - dry / wet / dB gains and their ramps, IrSlot switching (no ramp, end value)
//   - routing: per channel, true stereo LL LR RL RR, 3x3 matrix, modulo, fewer
//     channels in the call
//   - tail: exact to the frame, exact zeros afterwards, wake-up, EffectReset
//   - registry: generational handles, full, reuse, removed slot, shared IR, out of range
//   - blobs: round trip, every truncation, header corruption fuzz, unaligned
//   - IR rate conversion: impulse position, band-limited content, frame counts
//   - NaN / Inf recovery, dirty memory, higher runtime rate, batch and chains
//   - RT scopes and the cost of Process and EffectReset
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/convolution.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr double kPi = dsptest::kPi;
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kGuard = 512;
constexpr unsigned char kGuardByte = 0xA5;
using Multi = std::vector<std::vector<float>>;

std::vector<float> Noise(std::uint32_t n, std::uint32_t seed, float amp = 0.5f) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (float& x : v) x = nz.Next(amp);
  return v;
}

std::vector<float> DecayIr(std::uint32_t n, std::uint32_t seed, double tau) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (std::uint32_t i = 0; i < n; ++i)
    v[i] = static_cast<float>(nz.Next(1.0f) * 0.4 * std::exp(-static_cast<double>(i) / tau));
  return v;
}

// Prepared and registered IR (unregistered on destruction).
struct Ir {
  dsptest::AlignedMem mem;
  const ConvolutionIr* ir = nullptr;
  std::uint32_t slot = 0;
  Ir(const Multi& taps, std::uint32_t partition, std::uint32_t rate = kRate, float gain = 1.0f,
     std::uint32_t target = kRate, bool reg = true) {
    std::vector<const float*> p;
    for (const auto& c : taps) p.push_back(c.data());
    const ConvolutionIrDesc d{p.data(), static_cast<std::uint32_t>(taps.size()),
                              static_cast<std::uint32_t>(taps[0].size()), rate, gain};
    const std::size_t b = ConvolutionIrBytes(d, target, partition);
    if (b == 0u) return;
    mem.Reset(b, dsptest::kDirty);
    ir = ConvolutionPrepareIr(mem.get(), b, d, target, partition);
    if (ir != nullptr && reg) slot = ConvolutionRegisterIr(ir);
  }
  ~Ir() {
    if (slot != 0u) ConvolutionUnregisterIr(slot);
  }
  Ir(const Ir&) = delete;
  Ir& operator=(const Ir&) = delete;
};

struct Opt {
  float partition = 0.0f, zl = 1.0f, maxIrMs = 2000.0f, dry = 0.0f, wet = 1.0f, wetDb = 0.0f;
  std::uint32_t maxFrames = 256, rate = kRate;
  std::size_t bytes = 0;  // 0 = EffectStateBytes at `rate`
};

// Instance on dirty memory with a guard after the state.
struct Conv {
  std::vector<unsigned char> raw;
  unsigned char* p = nullptr;
  std::size_t bytes = 0;
  snd::EffectDesc desc{};
  float params[kMaxEffectParams] = {};
  bool ok = false;
  Conv(std::uint32_t channels, std::uint32_t slot, const Opt& o = Opt{}, int fill = dsptest::kDirty) {
    EffectDefaultParams(EffectKind::Convolution, params);
    params[kConvIrSlot] = static_cast<float>(slot);
    params[kConvDryMix] = o.dry;
    params[kConvWetMix] = o.wet;
    params[kConvWetGainDb] = o.wetDb;
    params[kConvPartitionFrames] = o.partition;
    params[kConvZeroLatency] = o.zl;
    params[kConvMaxIrMs] = o.maxIrMs;
    desc.effect_type = kEffectConvolution;
    desc.channels = channels;
    desc.max_frames = o.maxFrames;
    desc.param_count = kConvParamCount;
    desc.initial_params = params;
    bytes = o.bytes != 0u ? o.bytes : EffectStateBytes(desc, o.rate);
    raw.assign(bytes + kGuard + 64u, static_cast<unsigned char>(fill));
    p = raw.data() + (64u - reinterpret_cast<std::uintptr_t>(raw.data()) % 64u);
    std::memset(p + bytes, kGuardByte, kGuard);
    ok = bytes > 0u && EffectInit(p, bytes, desc, o.rate);
  }
  void* st() const { return p; }
  bool GuardIntact() const {
    for (std::size_t i = 0; i < kGuard; ++i)
      if (p[bytes + i] != kGuardByte) return false;
    return true;
  }
};

// Runs `in` through the instance in calls of the given sizes (cycled), in place.
// `pb` (optional) is given to every call.
Multi Run(const Conv& fx, const Multi& in, const std::vector<std::uint32_t>& sizes, std::uint32_t outCh = 0,
          const snd::ParamBlock* pb = nullptr) {
  const std::uint32_t ch = outCh != 0u ? outCh : static_cast<std::uint32_t>(in.size());
  const std::uint32_t n = static_cast<std::uint32_t>(in[0].size());
  Multi out(ch, std::vector<float>(n, 0.0f));
  std::uint32_t done = 0, k = 0;
  while (done < n) {
    std::uint32_t len = std::min(sizes[k++ % sizes.size()], n - done);
    Multi buf(ch, std::vector<float>(len, 0.0f));
    float* p[8];
    for (std::uint32_t c = 0; c < ch; ++c) {
      if (c < in.size()) std::memcpy(buf[c].data(), in[c].data() + done, 4u * len);
      p[c] = buf[c].data();
    }
    {
      SND_RT_SCOPE();
      EffectProcess(fx.st(), p, p, ch, len, pb, nullptr);
    }
    for (std::uint32_t c = 0; c < ch; ++c) std::memcpy(out[c].data() + done, buf[c].data(), 4u * len);
    done += len;
  }
  return out;
}

// y = x * h in double, delayed by `delay`; sparse in x (skips zero input samples).
std::vector<double> Direct(const std::vector<float>& x, const std::vector<float>& h, std::uint32_t delay = 0,
                           std::size_t outLen = 0) {
  const std::size_t n = outLen != 0u ? outLen : x.size();
  std::vector<double> y(n, 0.0);
  for (std::size_t t = 0; t < x.size(); ++t) {
    if (x[t] == 0.0f) continue;
    for (std::size_t k = 0; k < h.size() && t + k + delay < n; ++k)
      y[t + k + delay] += static_cast<double>(x[t]) * h[k];
  }
  return y;
}

double MaxErr(const std::vector<float>& got, const std::vector<double>& want, std::size_t* at = nullptr) {
  double e = 0.0;
  for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
    const double d = std::fabs(got[i] - want[i]);
    if (d > e) {
      e = d;
      if (at != nullptr) *at = i;
    }
  }
  return e;
}

double Peak(const std::vector<double>& v) {
  double p = 0.0;
  for (double x : v) p = std::max(p, std::fabs(x));
  return p;
}

// Error bound: float FFT convolution error grows with sqrt(taps) * |x| * |h|.
double Tol(const std::vector<float>& h, float xAmp) {
  double e = 0.0;
  for (float v : h) e += static_cast<double>(v) * v;
  return 2e-6 * (1.0 + std::sqrt(e) * xAmp * 4.0);
}

std::uint32_t NextPow2(std::uint32_t v) {
  std::uint32_t p = 1;
  while (p < v) p <<= 1u;
  return p;
}

}  // namespace

// C19 / C10: registration, parameter table, clamping and integer rounding.
SND_TEST(IndepConvolution, EffectInfoAndParameters) {
  const EffectInfo& info = GetEffectInfo(EffectKind::Convolution);
  SND_CHECK_EQ(static_cast<std::uint32_t>(EffectKind::Convolution), 14u);
  SND_CHECK_EQ(static_cast<std::uint32_t>(EffectKind::Count), 15u);
  SND_CHECK_EQ(info.typeId, 0x8837ECB0u);
  SND_CHECK_EQ(kEffectConvolution, 0x8837ECB0u);
  SND_CHECK(std::strcmp(info.name, "snd.fx.convolution") == 0);
  SND_CHECK_EQ(snd::HashId(info.name), info.typeId);
  SND_CHECK(FindEffect(0x8837ECB0u) == &info);
  SND_CHECK_EQ(info.paramCount, 7u);
  SND_CHECK_EQ(info.flags, static_cast<std::uint32_t>(kEffectHasLatency));
  struct P {
    const char* name;
    float lo, hi, def;
    std::uint32_t flags;
  };
  const P want[7] = {{"IrSlot", 0, 16777215, 0, kParamInteger},
                     {"DryMix", 0, 1, 0, 0},
                     {"WetMix", 0, 1, 1, 0},
                     {"WetGainDb", -48, 24, 0, 0},
                     {"PartitionFrames", 0, 4096, 0, kParamInitOnly | kParamInteger},
                     {"ZeroLatency", 0, 1, 1, kParamInitOnly | kParamInteger},
                     {"MaxIrMs", 10, 10000, 2000, kParamInitOnly}};
  for (std::uint32_t i = 0; i < 7; ++i) {
    const EffectParamInfo& p = info.params[i];
    SND_CHECK_MSG(std::strcmp(p.name, want[i].name) == 0 && p.minValue == want[i].lo && p.maxValue == want[i].hi &&
                      p.defaultValue == want[i].def && p.flags == want[i].flags,
                  "param %u (%s) differs from D-212", i, p.name);
  }
  float d[kMaxEffectParams] = {};
  EffectDefaultParams(EffectKind::Convolution, d);
  SND_CHECK(d[0] == 0.0f && d[1] == 0.0f && d[2] == 1.0f && d[3] == 0.0f && d[4] == 0.0f && d[5] == 1.0f &&
            d[6] == 2000.0f);
  // Clamping through the instance: out-of-range / NaN / fractional values.
  Conv fx(1, 0);
  SND_REQUIRE(fx.ok);
  SND_CHECK(EffectStateKind(fx.st()) == EffectKind::Convolution);
  const float s[7] = {2000.0f, -1.0f, std::nanf(""), 99.0f, 0.0f, 0.0f, 0.0f};
  const float e[7] = {3.6f, 7.0f, std::nanf(""), -INFINITY, 0.0f, 0.0f, 0.0f};
  snd::ParamBlock pb{s, e, 4, 0xF, nullptr};
  std::vector<float> buf(64, 0.0f);
  float* bp[1] = {buf.data()};
  EffectProcess(fx.st(), bp, bp, 1, 64, &pb, nullptr);
  SND_CHECK_EQ(EffectParamValue(fx.st(), kConvIrSlot), 4.0f);
  SND_CHECK_EQ(EffectParamValue(fx.st(), kConvDryMix), 1.0f);
  SND_CHECK_EQ(EffectParamValue(fx.st(), kConvWetMix), 1.0f);
  SND_CHECK_EQ(EffectParamValue(fx.st(), kConvWetGainDb), -48.0f);
}

// C1 / C15 / D-214: partition rule (auto and explicit, rounding and clamping),
// state size from MaxIrMs, the 64-byte multiple, latency report.
SND_TEST(IndepConvolution, PartitionRuleAndStateSize) {
  for (std::uint32_t mf : {1u, 63u, 64u, 65u, 200u, 256u, 1000u, 1024u, 1025u, 4096u, 9000u}) {
    for (float param : {0.0f, 0.4f, 1.0f, 63.0f, 64.0f, 100.0f, 1000.0f, 1025.0f, 3000.0f, 4096.0f, 5000.0f, -3.0f,
                        std::nanf("")}) {
      float pv = std::isnan(param) ? 0.0f : std::clamp(param, 0.0f, 4096.0f);
      pv = std::nearbyint(pv);
      std::uint32_t want;
      if (pv == 0.0f) want = std::clamp(NextPow2(mf), 64u, 1024u);
      else want = std::clamp(NextPow2(static_cast<std::uint32_t>(pv)), 64u, 4096u);
      Opt o;
      o.partition = param;
      o.maxFrames = mf;
      o.maxIrMs = 50.0f;
      Conv fx(2, 0, o);
      SND_REQUIRE(fx.ok);
      const std::uint32_t got = ConvolutionStatePartition(fx.st());
      const std::uint32_t pure = ConvolutionPartitionFrames(fx.desc);
      SND_CHECK_MSG(got == want && pure == want, "max_frames %u PartitionFrames %g: B %u / %u, want %u", mf, param,
                    got, pure, want);
    }
  }
  snd::EffectDesc bad{};
  bad.effect_type = kEffectReverb;
  bad.channels = 1;
  bad.max_frames = 256;
  SND_CHECK_EQ(ConvolutionPartitionFrames(bad), 0u);
  bad.effect_type = kEffectConvolution;
  bad.max_frames = 0;
  SND_CHECK_EQ(ConvolutionPartitionFrames(bad), 0u);
  // Size: cap = ceil(MaxIrMs * rate / 1000 / B); FDL = 8 * C * cap * B bytes.
  for (std::uint32_t ch : {1u, 2u, 8u})
    for (float ms : {10.0f, 333.0f, 2000.0f, 10000.0f})
      for (std::uint32_t b : {64u, 256u, 4096u})
        for (std::uint32_t rate : {22050u, 48000u, 96000u}) {
          Opt o;
          o.partition = static_cast<float>(b);
          o.maxIrMs = ms;
          o.rate = rate;
          snd::EffectDesc d{};
          float prm[kMaxEffectParams];
          EffectDefaultParams(EffectKind::Convolution, prm);
          prm[kConvPartitionFrames] = static_cast<float>(b);
          prm[kConvMaxIrMs] = ms;
          d.effect_type = kEffectConvolution;
          d.channels = ch;
          d.max_frames = 256;
          d.param_count = 7;
          d.initial_params = prm;
          const std::uint64_t bytes = EffectStateBytes(d, rate);
          const std::uint64_t cap = (static_cast<std::uint64_t>(std::ceil(ms * 0.001 * rate)) + b - 1u) / b;
          const std::uint64_t fdl = 8ull * ch * cap * b;
          SND_CHECK(bytes % 64u == 0u);
          const auto routes = ch * ch <= 16u ? ch * ch : ch;
          const auto taps = 5ull * routes * b * sizeof(float);
          // D-214: five tap banks plus the input delay line, additional
          // O(ch*B) workspace and the FFT plan. Keep the previous upper bound.
          SND_CHECK_MSG(bytes >= fdl + taps && bytes <= fdl + 64ull * ch * b + 200000ull,
                        "ch %u %g ms B %u rate %u: %llu bytes, FDL %llu", ch, ms, b, rate,
                        static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(fdl));
        }
  // D-214 example: stereo, 48 kHz, 2 s, B 256: about 1.55 MB.
  Opt o;
  o.partition = 256;
  Conv ex(2, 0, o);
  snd::test::Note("state stereo 48k 2 s B256: %llu bytes", static_cast<unsigned long long>(ex.bytes));
  SND_CHECK(ex.bytes > 1500000u && ex.bytes < 1650000u);
  SND_CHECK_EQ(ConvolutionStateCapacityFrames(ex.st()), 96000u + 256u);
  // 1 GiB limit: a huge sizing rate is capped.
  snd::EffectDesc d{};
  float prm[kMaxEffectParams];
  EffectDefaultParams(EffectKind::Convolution, prm);
  prm[kConvMaxIrMs] = 10000.0f;
  prm[kConvPartitionFrames] = 64.0f;
  d.effect_type = kEffectConvolution;
  d.channels = 8;
  d.max_frames = 64;
  d.param_count = 7;
  d.initial_params = prm;
  const std::uint64_t huge = EffectStateBytes(d, 4000000u);
  snd::test::Note("8 ch, 10 s, B 64 at 4 MHz: %llu bytes", static_cast<unsigned long long>(huge));
  SND_CHECK(huge > 0u && huge <= (1ull << 30) + 4u * 1024u * 1024u);
  // Latency report.
  Opt z;
  z.zl = 0.0f;
  z.partition = 512;
  Conv l0(1, 0, z);
  SND_CHECK_EQ(EffectLatencyFrames(l0.st()), 512u);
  Conv l1(1, 0);
  SND_CHECK_EQ(EffectLatencyFrames(l1.st()), 0u);
}

// C2: impulse IRs at delays 0, 1, B-1, B, B+1, 2B-1, 2B, 5B+3 (and a gain):
// output = gain * input delayed (exactly when the impulse is in the direct head
// and the latency is 0). ZeroLatency 0: + B frames, and the DRY path is delayed
// by B too (bit exact).
SND_TEST(IndepConvolution, ImpulseIrsAndLatency) {
  const std::vector<float> x = Noise(9000, 3);
  for (std::uint32_t b : {64u, 128u}) {
    for (std::uint32_t d : {0u, 1u, b - 1u, b, b + 1u, 2u * b - 1u, 2u * b, 5u * b + 3u}) {
      std::vector<float> h(d + 1u, 0.0f);
      h[d] = 0.5f;
      Ir ir({h}, b);
      SND_REQUIRE(ir.slot != 0u);
      for (float zl : {1.0f, 0.0f}) {
        Opt o;
        o.partition = static_cast<float>(b);
        o.zl = zl;
        Conv fx(1, ir.slot, o);
        SND_REQUIRE(fx.ok);
        const std::uint32_t lat = zl != 0.0f ? 0u : b;
        SND_CHECK_EQ(EffectLatencyFrames(fx.st()), lat);
        const Multi y = Run(fx, {x}, {100, 37, 256, 1});
        double e = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
          const double want = i >= d + lat ? 0.5 * x[i - d - lat] : 0.0;
          e = std::max(e, std::fabs(y[0][i] - want));
        }
        if (zl != 0.0f && d < b) SND_CHECK_MSG(e == 0.0, "B %u d %u: head not exact (%g)", b, d, e);
        else SND_CHECK_MSG(e < 1e-6, "B %u d %u zl %g: error %g", b, d, static_cast<double>(zl), e);
      }
    }
    // Dry only, ZeroLatency 0: the input delayed by exactly B, bit for bit.
    Opt o;
    o.partition = static_cast<float>(b);
    o.zl = 0.0f;
    o.dry = 1.0f;
    o.wet = 0.0f;
    Conv fx(1, 0, o);
    const Multi y = Run(fx, {x}, {77});
    bool exact = true;
    for (std::size_t i = 0; i < x.size(); ++i) exact = exact && y[0][i] == (i >= b ? x[i - b] : 0.0f);
    SND_CHECK_MSG(exact, "B %u: delayed dry is not exact", b);
    o.zl = 1.0f;
    Conv fx1(1, 0, o);
    SND_CHECK(Run(fx1, {x}, {77})[0] == x);
  }
}

// C3 / C4: IR lengths around every partition boundary and the direct head,
// against a direct convolution in double; block sizes 1, B-1, B, B+1, 97,
// variable and larger than max_frames give the SAME bits.
SND_TEST(IndepConvolution, PartitionBoundariesMatchDirectConvolution) {
  const std::vector<float> x = Noise(6000, 17);
  for (std::uint32_t b : {64u, 256u}) {
    for (std::uint32_t taps : {1u, b - 1u, b, b + 1u, 2u * b - 1u, 2u * b, 2u * b + 1u, 3u * b, 7u * b + 5u}) {
      const std::vector<float> h = DecayIr(taps, taps * 7u + b, taps / 3.0 + 1.0);
      Ir ir({h}, b);
      SND_REQUIRE(ir.slot != 0u);
      for (float zl : {1.0f, 0.0f}) {
        Opt o;
        o.partition = static_cast<float>(b);
        o.zl = zl;
        o.maxFrames = 256;
        Conv ref(1, ir.slot, o);
        const Multi y = Run(ref, {x}, {256});
        const std::vector<double> want = Direct(x, h, zl != 0.0f ? 0u : b);
        std::size_t at = 0;
        const double e = MaxErr(y[0], want, &at);
        SND_CHECK_MSG(e < Tol(h, 0.5f), "B %u taps %u zl %g: error %g at %llu (tol %g)", b, taps,
                      static_cast<double>(zl), e, static_cast<unsigned long long>(at), Tol(h, 0.5f));
        SND_CHECK_EQ(ConvolutionStateActiveIrFrames(ref.st()), taps);
        for (const std::vector<std::uint32_t>& sizes :
             {std::vector<std::uint32_t>{1}, std::vector<std::uint32_t>{b - 1u}, std::vector<std::uint32_t>{b},
              std::vector<std::uint32_t>{b + 1u}, std::vector<std::uint32_t>{97},
              std::vector<std::uint32_t>{1000, 3, 255, 257, 64, 513}}) {
          Conv fx(1, ir.slot, o);
          const Multi z = Run(fx, {x}, sizes);
          SND_CHECK_MSG(z == y, "B %u taps %u zl %g: block size %u changes the bits", b, taps,
                        static_cast<double>(zl), sizes[0]);
        }
      }
    }
  }
}

// C3: a 2 s stereo IR (true per channel) with a 50 ms burst of input, 2.5 s of
// output compared with the direct convolution everywhere (every partition
// boundary of the FDL is crossed by the tail).
SND_TEST(IndepConvolution, LongIrMatchesDirectConvolution) {
  const std::uint32_t taps = 96000;
  const Multi h = {DecayIr(taps, 1, 20000.0), DecayIr(taps, 2, 20000.0)};
  Multi x(2, std::vector<float>(120000, 0.0f));
  for (std::uint32_t c = 0; c < 2; ++c) {
    const std::vector<float> burst = Noise(2400, 50u + c);
    std::copy(burst.begin(), burst.end(), x[c].begin() + 1000);
  }
  for (std::uint32_t b : {256u, 1024u}) {
    Ir ir(h, b);
    SND_REQUIRE(ir.slot != 0u);
    for (float zl : {1.0f, 0.0f}) {
      Opt o;
      o.partition = static_cast<float>(b);
      o.zl = zl;
      o.maxIrMs = 2000.0f;
      Conv fx(2, ir.slot, o);
      SND_REQUIRE(fx.ok);
      const Multi y = Run(fx, x, {256});
      SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), taps);
      for (std::uint32_t c = 0; c < 2; ++c) {
        const std::vector<double> want = Direct(x[c], h[c], zl != 0.0f ? 0u : b);
        std::size_t at = 0;
        const double e = MaxErr(y[c], want, &at);
        snd::test::Note("2 s IR, B %u zl %g ch %u: max error %.3g at %llu (peak %.3g)", b, static_cast<double>(zl), c,
                        e, static_cast<unsigned long long>(at), Peak(want));
        SND_CHECK(e < 2e-5 * (1.0 + Peak(want)));
      }
      SND_CHECK(fx.GuardIntact());
    }
  }
}

// D-215: dry / wet / dB gains, constant and ramped (value at frame j of an
// n-frame call = start + (end - start) * j / n), and gains outside the ranges.
SND_TEST(IndepConvolution, DryWetAndRamps) {
  const std::vector<float> x = Noise(4096, 8);
  const std::vector<float> h = DecayIr(700, 9, 150.0);
  Ir ir({h}, 64);
  const std::vector<double> wet = Direct(x, h);
  {
    Opt o;
    o.partition = 64;
    o.dry = 0.3f;
    o.wet = 0.6f;
    o.wetDb = -6.0f;
    Conv fx(1, ir.slot, o);
    const Multi y = Run(fx, {x}, {256});
    const double gw = 0.6 * std::pow(10.0, -6.0 / 20.0);
    double e = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) e = std::max(e, std::fabs(y[0][i] - (0.3 * x[i] + gw * wet[i])));
    SND_CHECK_MSG(e < Tol(h, 0.5f), "constant gains: error %g", e);
  }
  {
    Opt o;
    o.partition = 64;
    Conv fx(1, ir.slot, o);
    // Every call of 512 frames ramps DryMix 0 -> 1, WetMix 1 -> 0.5, WetGainDb 0 -> 12.
    const float s[7] = {static_cast<float>(ir.slot), 0.0f, 1.0f, 0.0f, 0, 1, 2000};
    const float e7[7] = {static_cast<float>(ir.slot), 1.0f, 0.5f, 12.0f, 0, 1, 2000};
    snd::ParamBlock pb{s, e7, 7, 0xE, nullptr};
    const Multi y = Run(fx, {x}, {512}, 0, &pb);
    double e = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double t = static_cast<double>(i % 512u) / 512.0;
      const double gd = t, gw = (1.0 - 0.5 * t) * std::pow(10.0, 12.0 * t / 20.0);
      e = std::max(e, std::fabs(y[0][i] - (gd * x[i] + gw * wet[i])));
    }
    SND_CHECK_MSG(e < 4.0 * Tol(h, 0.5f) + 1e-6, "ramped gains: error %g", e);
  }
  {
    // WetGainDb +24 (max) and beyond (clamped to +24).
    Opt o;
    o.partition = 64;
    o.wetDb = 40.0f;
    Conv fx(1, ir.slot, o);
    const std::vector<float> xs = Noise(2000, 4, 0.01f);
    const Multi y = Run(fx, {xs}, {256});
    const std::vector<double> w = Direct(xs, h);
    const double g = std::pow(10.0, 24.0 / 20.0);
    double e = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) e = std::max(e, std::fabs(y[0][i] - g * w[i]));
    SND_CHECK(e < 1e-4);
  }
}

// D-215: routing. Per channel, true stereo (LL LR RL RR), 3x3 matrix, modulo
// (I = 3 on C = 2, I = 2 on C = 4, I = 4 on C = 1), and a call with fewer
// channels than the instance (missing inputs are silence).
SND_TEST(IndepConvolution, Routing) {
  const std::uint32_t b = 64;
  Multi irs;
  for (std::uint32_t i = 0; i < 9; ++i) {
    std::vector<float> h(300 + 37 * i, 0.0f);
    h[3 + 50 * i] = 0.25f + 0.05f * i;  // distinct delay and gain per IR channel
    h[h.size() - 1] = -0.1f * (i + 1);
    irs.push_back(h);
  }
  const auto take = [&](std::uint32_t n) { return Multi(irs.begin(), irs.begin() + n); };
  struct Case {
    std::uint32_t I, C, callCh;
  };
  for (const Case cs : {Case{2, 2, 2}, Case{4, 2, 2}, Case{9, 3, 3}, Case{1, 4, 4}, Case{3, 2, 2}, Case{2, 4, 4},
                        Case{4, 1, 1}, Case{4, 2, 1}, Case{2, 2, 1}, Case{1, 3, 2}}) {
    // An IR of I channels must have the same length per channel: pad.
    Multi h = take(cs.I);
    std::size_t len = 0;
    for (auto& c : h) len = std::max(len, c.size());
    for (auto& c : h) c.resize(len, 0.0f);
    Ir ir(h, b);
    SND_REQUIRE(ir.slot != 0u);
    Opt o;
    o.partition = b;
    Conv fx(cs.C, ir.slot, o);
    Multi x;
    for (std::uint32_t c = 0; c < cs.callCh; ++c) x.push_back(Noise(3000, 60u + c));
    const Multi y = Run(fx, x, {256});
    for (std::uint32_t oc = 0; oc < cs.callCh; ++oc) {
      std::vector<double> want(3000, 0.0);
      const auto add = [&](std::uint32_t in, std::uint32_t irCh) {
        if (in >= cs.callCh) return;  // missing input = silence
        const std::vector<double> w = Direct(x[in], h[irCh]);
        for (std::size_t i = 0; i < want.size(); ++i) want[i] += w[i];
      };
      if (cs.I == cs.C) add(oc, oc);
      else if (cs.C >= 2u && cs.I == cs.C * cs.C)
        for (std::uint32_t in = 0; in < cs.C; ++in) add(in, in * cs.C + oc);
      else add(oc, oc % cs.I);
      const double e = MaxErr(y[oc], want);
      SND_CHECK_MSG(e < 2e-6, "I %u C %u call %u out %u: error %g", cs.I, cs.C, cs.callCh, oc, e);
    }
  }
}

// IrSlot: the END value of a call selects the IR (no ramp), switching keeps the
// history (the new IR applies to the recent input at once); a slot holding an
// IR of another B or rate, an empty slot and an unregistered slot give a silent
// wet; two instances share one IR.
SND_TEST(IndepConvolution, IrSlotSelectionAndSharing) {
  const std::uint32_t b = 64;
  const std::vector<float> h1 = DecayIr(500, 1, 100.0), h2 = DecayIr(900, 2, 300.0);
  Ir a({h1}, b), c({h2}, b), wrongB({h1}, 128), wrongRate({h1}, b, kRate, 1.0f, 44100);
  SND_REQUIRE(a.slot && c.slot && wrongB.slot && wrongRate.slot);
  const std::vector<float> x = Noise(4000, 5);
  Opt o;
  o.partition = b;
  o.dry = 0.5f;
  // Switch at frame 1984 from a to c with a ParamBlock whose start is a, end is c.
  Conv fx(1, a.slot, o);
  std::vector<float> y(4000);
  {
    std::vector<float> buf(x.begin(), x.begin() + 1984);
    float* p[1] = {buf.data()};
    EffectProcess(fx.st(), p, p, 1, 1984, nullptr, nullptr);
    std::copy(buf.begin(), buf.end(), y.begin());
    const float s[1] = {static_cast<float>(a.slot)}, e[1] = {static_cast<float>(c.slot)};
    snd::ParamBlock pb{s, e, 1, 1, nullptr};
    // The first call requests a -> c; the whole call selects c.
    for (std::uint32_t done = 1984; done < 4000;) {
      const auto len = std::min(250u, 4000u - done);
      std::vector<float> buf2(x.begin() + done, x.begin() + done + len);
      float* p2[1] = {buf2.data()};
      EffectProcess(fx.st(), p2, p2, 1, len, done == 1984 ? &pb : nullptr, nullptr);
      std::copy(buf2.begin(), buf2.end(), y.begin() + done);
      done += len;
    }
  }
  const std::vector<double> w1 = Direct(x, h1), w2 = Direct(x, h2);
  double e = 0.0;
  std::size_t eAt = 0;
  // The switch starts at offset 0 of B=64. The linear blend reaches 1
  // on frame 2047 and the following frames match the new IR alone.
  for (std::size_t i = 0; i < 4000; ++i) {
    const double g = i < 1984 ? 0.0 : std::min(1.0, double(i - 1984 + 1) / b);
    const double d = std::fabs(y[i] - (0.5 * x[i] + (1 - g) * w1[i] + g * w2[i]));
    if (d > e) {
      e = d;
      eAt = i;
    }
  }
  SND_CHECK_MSG(e < Tol(h2, 0.5f), "IrSlot switch: error %g at %llu (the new IR must apply to the history at once)", e,
                static_cast<unsigned long long>(eAt));
  SND_CHECK_EQ(ConvolutionStateActiveIrFrames(fx.st()), 900u);
  // Unusable / absent IRs: wet silent, dry passes.
  for (std::uint32_t slot : {wrongB.slot, wrongRate.slot, 0u, kConvolutionMaxIrNumber + 1u}) {
    Conv g(1, slot, o);
    const Multi z = Run(g, {x}, {256});
    bool dryOnly = true;
    for (std::size_t i = 0; i < x.size(); ++i) dryOnly = dryOnly && z[0][i] == 0.5f * x[i];
    SND_CHECK_MSG(dryOnly, "slot %u: wet is not silent", slot);
    SND_CHECK_EQ(ConvolutionStateActiveIrFrames(g.st()), 0u);
  }
  // Sharing: two instances on the same slot give the same bits as each alone.
  Conv s1(1, a.slot, o), s2(1, a.slot, o), alone(1, a.slot, o);
  const std::vector<float> x2 = Noise(4000, 6);
  Multi y1(1, std::vector<float>(4000)), y2(1, std::vector<float>(4000));
  for (std::uint32_t done = 0; done < 4000; done += 200) {
    std::vector<float> b1(x.begin() + done, x.begin() + done + 200), b2(x2.begin() + done, x2.begin() + done + 200);
    float* p1[1] = {b1.data()};
    float* p2[1] = {b2.data()};
    EffectProcess(s1.st(), p1, p1, 1, 200, nullptr, nullptr);
    EffectProcess(s2.st(), p2, p2, 1, 200, nullptr, nullptr);
    std::copy(b1.begin(), b1.end(), y1[0].begin() + done);
    std::copy(b2.begin(), b2.end(), y2[0].begin() + done);
  }
  SND_CHECK(Run(alone, {x}, {200}) == y1);
  // Removing the IR of a running instance (after its calls): dry only (after the
  // current fade has finished).
  {
    Ir t({h1}, b);
    Conv g(1, t.slot, o);
    Run(g, {x}, {256});
    SND_REQUIRE(ConvolutionUnregisterIr(t.slot));
    const Multi z = Run(g, {x}, {256});
    bool dryOnly = true;
    for (std::size_t i = b; i < x.size(); ++i) dryOnly = dryOnly && z[0][i] == 0.5f * x[i];
    SND_CHECK(dryOnly);
    t.slot = 0;
  }
}

// Reserve only vacant entries in the process-wide table. Once full, freeing
// exactly one of our entries forces reuse of its physical slot.
SND_TEST(IndepConvolution, Registry) {
  Ir a({{1.0f}}, 64, kRate, 1.0f, kRate, false);
  Ir foreign({{0.5f}}, 64);
  SND_REQUIRE(a.ir != nullptr && foreign.slot != 0u);
  SND_CHECK_EQ(ConvolutionRegisterIr(nullptr), 0u);
  alignas(64) unsigned char junk[256];
  std::memset(junk, 0x11, sizeof(junk));
  SND_CHECK_EQ(ConvolutionRegisterIr(reinterpret_cast<const ConvolutionIr*>(junk)), 0u);
  constexpr unsigned capacity = 1024;  // D-219: ten physical slot bits
  std::array<bool, capacity> occupied{};
  occupied[foreign.slot & 1023u] = true;
  std::vector<std::uint32_t> owned;
  for (std::size_t i = 0; i < kConvolutionMaxIrSlots; ++i) {
    const auto id = ConvolutionRegisterIr(a.ir);
    if (id == 0u) break;
    SND_CHECK(id <= 16777215u);
    SND_CHECK(ConvolutionRegisteredIr(id) == a.ir);
    const unsigned slot = id & 1023u;
    SND_CHECK(!occupied[slot]);
    occupied[slot] = true;
    owned.push_back(id);
  }
  SND_REQUIRE(!owned.empty());
  SND_CHECK(owned.size() < kConvolutionMaxIrSlots);
  SND_CHECK_EQ(ConvolutionRegisterIr(a.ir), 0u);
  // Other tests may own entries. Resolve their possible generations without
  // unregistering them. A prematurely full table still has a missing slot.
  for (unsigned slot = 0; slot < capacity; ++slot) {
    if (occupied[slot]) continue;
    for (unsigned id = slot; id < (1u << 24); id += capacity) {
      if (ConvolutionRegisteredIr(id) != nullptr) {
        occupied[slot] = true;
        break;
      }
    }
  }
  SND_CHECK_EQ(std::count(occupied.begin(), occupied.end(), true), 1024);
  const auto old = owned.back();
  SND_CHECK(ConvolutionUnregisterIr(old));
  SND_CHECK(ConvolutionRegisteredIr(old) == nullptr);
  const auto fresh = ConvolutionRegisterIr(a.ir);
  SND_CHECK(fresh != 0u && fresh != old && (fresh & 1023u) == (old & 1023u));
  SND_CHECK(!ConvolutionUnregisterIr(old));
  SND_CHECK(ConvolutionRegisteredIr(old) == nullptr);
  SND_CHECK(ConvolutionRegisteredIr(fresh) == a.ir);
  owned.back() = fresh;
  for (auto id : owned) SND_CHECK(ConvolutionUnregisterIr(id));
  SND_CHECK(ConvolutionRegisteredIr(foreign.slot) == foreign.ir);
  for (auto id : {0u, 16777216u, UINT32_MAX}) {
    SND_CHECK(!ConvolutionUnregisterIr(id));
    SND_CHECK(ConvolutionRegisteredIr(id) == nullptr);
  }
  // Queries on invalid IRs.
  SND_CHECK_EQ(ConvolutionIrChannels(nullptr), 0u);
  SND_CHECK_EQ(ConvolutionIrFrames(reinterpret_cast<const ConvolutionIr*>(junk)), 0u);
  SND_CHECK_EQ(ConvolutionIrPartition(a.ir), 64u);
  SND_CHECK_EQ(ConvolutionIrSampleRate(a.ir), kRate);
}

// C11: concurrent registration from several threads never hands out the same
// slot twice, and RT reads during register / unregister see either nullptr or
// a valid IR (TSan: no data race). Instances keep processing an IR in a stable
// slot while other slots churn.
SND_TEST(IndepConvolution, RegistryConcurrency) {
  Ir stable({DecayIr(300, 4, 80.0)}, 64);
  SND_REQUIRE(stable.slot != 0u);
  Ir other({std::vector<float>{0.5f, 0.25f}}, 64, kRate, 1.0f, kRate, false);
  constexpr int kThreads = 4, kIters = 400;
  std::vector<std::vector<std::uint32_t>> taken(kThreads);
  std::atomic<int> badRead{0}, dup{0};
  std::atomic<bool> stop{false}, stableReadDuringWork{false};
  std::atomic<std::uint32_t> published[kThreads]{};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (published[0].load(std::memory_order_acquire) != 0u) {
        if (ConvolutionRegisteredIr(stable.slot) != stable.ir) ++badRead;
        stableReadDuringWork.store(true, std::memory_order_release);
      }
      for (auto& number : published) {
        const auto id = number.load(std::memory_order_acquire);
        const ConvolutionIr* p = ConvolutionRegisteredIr(id);
        if (p != nullptr && (p != other.ir || ConvolutionIrPartition(p) != 64u)) ++badRead;
      }
    }
  });
  Opt o;
  o.partition = 64;
  o.dry = 0.0f;
  Conv fx(1, stable.slot, o);
  std::thread audio([&] {
    std::vector<float> buf(256);
    float* p[1] = {buf.data()};
    dsptest::Noise nz(1);
    while (!stop.load(std::memory_order_relaxed)) {
      for (float& v : buf) v = nz.Next(0.5f);
      EffectProcess(fx.st(), p, p, 1, 256, nullptr, nullptr);
    }
  });
  std::vector<std::thread> workers;
  std::atomic<int> slotsInUse[kConvolutionMaxIrSlots + 1];
  for (auto& s : slotsInUse) s.store(0);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < kIters; ++i) {
        const std::uint32_t s = ConvolutionRegisterIr(other.ir);
        if (s == 0u) continue;
        if (slotsInUse[s & 1023u].fetch_add(1) != 0) ++dup;
        published[t].store(s, std::memory_order_release);
        if (t == 0 && i == 0) {
          while (!stableReadDuringWork.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        if (ConvolutionRegisteredIr(s) != other.ir) ++badRead;
        if (s == stable.slot) ++dup;
        taken[t].push_back(s);
        if (i % 3 == 2) {
          const std::uint32_t r = taken[t].back();
          taken[t].pop_back();
          slotsInUse[r & 1023u].fetch_sub(1);
          if (!ConvolutionUnregisterIr(r)) ++dup;
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  stop.store(true);
  reader.join();
  audio.join();
  SND_CHECK(stableReadDuringWork.load());
  SND_CHECK_EQ(dup.load(), 0);
  SND_CHECK_EQ(badRead.load(), 0);
  SND_CHECK(ConvolutionRegisteredIr(stable.slot) == stable.ir);
  for (auto& v : taken)
    for (std::uint32_t s : v) SND_CHECK(ConvolutionUnregisterIr(s));

}

// C8: the tail. Done exactly after cap*B + 2B (+B with ZeroLatency 0) frames
// below -100 dBFS after the last loud frame, whatever the call sizes; the
// output during the tail is the convolution; afterwards silent input gives
// exact zeros; new input wakes the effect and is convolved alone; EffectReset
// leaves the tail done.
SND_TEST(IndepConvolution, TailExactAndWakeUp) {
  const std::uint32_t b = 64;
  const std::vector<float> h = DecayIr(1000, 7, 200.0);
  Ir ir({h}, b);
  for (float zl : {1.0f, 0.0f}) {
    for (const std::vector<std::uint32_t>& sizes :
         {std::vector<std::uint32_t>{256}, std::vector<std::uint32_t>{1}, std::vector<std::uint32_t>{97, 3, 640}}) {
      Opt o;
      o.partition = b;
      o.zl = zl;
      o.maxIrMs = 30.0f;  // cap = ceil(1440 / 64) = 23
      Conv fx(1, ir.slot, o);
      constexpr std::uint32_t cap = (1440u + b - 1u) / b;
      SND_CHECK_EQ(ConvolutionStateCapacityFrames(fx.st()), (cap + (zl == 1.0f ? 1u : 0u)) * b);
      SND_CHECK(EffectIsTailDone(fx.st()));
      const std::uint32_t tail = cap * b + 2u * b + (zl != 0.0f ? 0u : b);
      // 500 loud frames, the last loud frame at 499 (then 1e-6 noise: below -100 dBFS).
      std::vector<float> x(500 + tail + 3000, 0.0f);
      const std::vector<float> loud = Noise(500, 9);
      std::copy(loud.begin(), loud.end(), x.begin());
      dsptest::Noise nz(2);
      for (std::size_t i = 500; i < 500 + tail; ++i) x[i] = nz.Next(9e-6f);
      // Process frame-exactly up to the predicted end of the tail.
      Multi y(1, std::vector<float>(x.size(), 0.0f));
      std::uint32_t done = 0, k = 0;
      bool doneEarly = false, doneLate = false;
      while (done < x.size()) {
        std::uint32_t len = std::min<std::uint32_t>(sizes[k++ % sizes.size()], static_cast<std::uint32_t>(x.size()) - done);
        const std::uint32_t boundary = 500u + tail;
        if (done < boundary - 1u && done + len > boundary - 1u) len = boundary - 1u - done;
        else if (done == boundary - 1u) len = 1u;
        std::vector<float> buf(x.begin() + done, x.begin() + done + len);
        float* p[1] = {buf.data()};
        EffectProcess(fx.st(), p, p, 1, len, nullptr, nullptr);
        std::copy(buf.begin(), buf.end(), y[0].begin() + done);
        done += len;
        if (done == boundary - 1u && EffectIsTailDone(fx.st())) doneEarly = true;
        if (done == boundary && !EffectIsTailDone(fx.st())) doneLate = true;
        if (done > 500u && done < boundary - 1u && EffectIsTailDone(fx.st())) doneEarly = true;
      }
      SND_CHECK_MSG(!doneEarly && !doneLate, "zl %g sizes %u: tail not exact (early %d late %d, want %u frames)",
                    static_cast<double>(zl), sizes[0], doneEarly, doneLate, tail);
      const std::vector<double> want = Direct(x, h, zl != 0.0f ? 0u : b);
      double e = 0.0;
      for (std::size_t i = 0; i < 500u + tail - 2u * b; ++i) e = std::max(e, std::fabs(y[0][i] - want[i]));
      SND_CHECK_MSG(e < Tol(h, 0.5f), "tail output error %g", e);
      bool zeros = true;
      for (std::size_t i = 500u + tail; i < x.size(); ++i) zeros = zeros && y[0][i] == 0.0f;
      SND_CHECK_MSG(zeros, "zl %g: silent input after the tail is not exact zeros", static_cast<double>(zl));
      // Wake up: a new burst is convolved alone (history below -100 dBFS is dropped).
      const std::vector<float> x2 = Noise(1500, 33);
      std::vector<float> xin(x2);
      xin.resize(4000, 0.0f);
      const Multi y2 = Run(fx, {xin}, sizes);
      const std::vector<double> want2 = Direct(xin, h, zl != 0.0f ? 0u : b);
      SND_CHECK_MSG(MaxErr(y2[0], want2) < Tol(h, 0.5f) + 1e-4, "zl %g: wake-up output error %g",
                    static_cast<double>(zl), MaxErr(y2[0], want2));
      EffectReset(fx.st());
      SND_CHECK(EffectIsTailDone(fx.st()));
    }
  }
}

// D-213: blobs. Round trip, unaligned blob, every truncation and every
// single-header-field corruption is either rejected or consistent (ASan:
// never reads outside the blob), and IrBytesFromBlob / PrepareFromBlob agree
// with the parse.
SND_TEST(IndepConvolution, BlobFuzz) {
  const Multi h = {DecayIr(300, 1, 50.0), DecayIr(300, 2, 50.0)};
  const float* p[2] = {h[0].data(), h[1].data()};
  const std::size_t need = 32u + 4u * 2u * 300u;
  SND_CHECK_EQ(ConvolutionWriteIrBlob(nullptr, need, p, 2, 300, 44100, 0.5f), 0u);
  std::vector<unsigned char> blob(need);
  SND_CHECK_EQ(ConvolutionWriteIrBlob(blob.data(), need - 1u, p, 2, 300, 44100, 0.5f), 0u);
  SND_CHECK_EQ(ConvolutionWriteIrBlob(blob.data(), need, p, 2, 300, 44100, 0.5f), need);
  SND_CHECK_EQ(ConvolutionWriteIrBlob(blob.data(), need, p, 0, 300, 44100, 0.5f), 0u);
  SND_CHECK_EQ(ConvolutionWriteIrBlob(blob.data(), need, p, 17, 300, 44100, 0.5f), 0u);
  SND_CHECK_EQ(ConvolutionWriteIrBlob(blob.data(), need, p, 2, 0, 44100, 0.5f), 0u);
  // Header layout, little endian.
  std::uint32_t hd[8];
  std::memcpy(hd, blob.data(), 32);
  float g;
  std::memcpy(&g, &hd[5], 4);
  SND_CHECK(hd[0] == 0x52494353u && hd[1] == 1u && hd[2] == 2u && hd[3] == 300u && hd[4] == 44100u && g == 0.5f &&
            hd[6] == 0u && hd[7] == 0u);
  SND_CHECK(std::memcmp(blob.data() + 32, h[0].data(), 1200) == 0 && std::memcmp(blob.data() + 1232, h[1].data(), 1200) == 0);
  ConvolutionBlobInfo bi{};
  SND_CHECK(ConvolutionParseIrBlob(blob.data(), need, &bi));
  SND_CHECK(bi.channels == 2u && bi.frames == 300u && bi.sampleRate == 44100u && bi.gain == 0.5f &&
            bi.samples == blob.data() + 32);
  // Unaligned copy prepares the same IR as from the samples.
  std::vector<unsigned char> un(need + 3u);
  std::memcpy(un.data() + 3, blob.data(), need);
  const std::size_t irb = ConvolutionIrBytesFromBlob(un.data() + 3, need, kRate, 64);
  SND_REQUIRE(irb > 0u);
  dsptest::AlignedMem m1(irb), m2(irb);
  const ConvolutionIr* a = ConvolutionPrepareIrFromBlob(m1.get(), irb, un.data() + 3, need, kRate, 64);
  const ConvolutionIrDesc d{p, 2, 300, 44100, 0.5f};
  SND_CHECK_EQ(ConvolutionIrBytes(d, kRate, 64), irb);
  const ConvolutionIr* b = ConvolutionPrepareIr(m2.get(), irb, d, kRate, 64);
  SND_REQUIRE(a != nullptr && b != nullptr);
  SND_CHECK(std::memcmp(m1.get(), m2.get(), irb) == 0);
  SND_CHECK(ConvolutionPrepareIrFromBlob(m1.get(), irb - 1u, blob.data(), need, kRate, 64) == nullptr);
  // Every truncation (exact-size heap copies so ASan sees any over-read).
  for (std::size_t n = 0; n < need; n += (n < 64u ? 1u : 97u)) {
    std::vector<unsigned char> t(blob.begin(), blob.begin() + n);
    SND_CHECK_MSG(!ConvolutionParseIrBlob(n != 0u ? t.data() : nullptr, n, &bi) && ConvolutionIrBytesFromBlob(t.data(), n, kRate, 64) == 0u,
                  "truncated blob of %llu bytes accepted", static_cast<unsigned long long>(n));
  }
  std::vector<unsigned char> longer(need + 1u, 0);
  std::memcpy(longer.data(), blob.data(), need);
  SND_CHECK(!ConvolutionParseIrBlob(longer.data(), longer.size(), &bi));
  SND_CHECK(!ConvolutionParseIrBlob(nullptr, need, &bi));
  // Field corruptions.
  const auto with = [&](std::uint32_t field, std::uint32_t value) {
    std::vector<unsigned char> c(blob);
    std::memcpy(c.data() + 4u * field, &value, 4);
    return c;
  };
  struct F {
    std::uint32_t field, value;
  };
  for (const F f : {F{0, 0x52494354u}, F{1, 0u}, F{1, 2u}, F{2, 0u}, F{2, 17u}, F{2, 1u}, F{2, 0xFFFFFFFFu}, F{3, 0u},
                    F{3, 301u}, F{3, 0xFFFFFFFFu}, F{3, 0x40000000u}, F{4, 0u}, F{4, 384001u}, F{6, 1u}, F{7, 5u}}) {
    const std::vector<unsigned char> c = with(f.field, f.value);
    SND_CHECK_MSG(!ConvolutionParseIrBlob(c.data(), c.size(), &bi), "field %u = %u accepted", f.field, f.value);
    SND_CHECK(ConvolutionPrepareIrFromBlob(m1.get(), irb, c.data(), c.size(), kRate, 64) == nullptr);
  }
  for (float bad : {std::nanf(""), INFINITY, -INFINITY}) {
    std::uint32_t v;
    std::memcpy(&v, &bad, 4);
    const std::vector<unsigned char> c = with(5, v);
    SND_CHECK(!ConvolutionParseIrBlob(c.data(), c.size(), &bi));
    auto samples = blob;
    std::memcpy(samples.data() + need - 4, &v, 4);
    SND_CHECK(!ConvolutionParseIrBlob(samples.data(), need, &bi));
    SND_CHECK(ConvolutionPrepareIrFromBlob(m1.get(), irb, samples.data(), need, kRate, 64) == nullptr);
  }
  // Channels 1 with 600 frames reinterprets the same bytes consistently: accepted.
  {
    std::vector<unsigned char> c = with(2, 1u);
    std::uint32_t f600 = 600u;
    std::memcpy(c.data() + 12, &f600, 4);
    SND_CHECK(ConvolutionParseIrBlob(c.data(), c.size(), &bi) && bi.frames == 600u);
  }
  // Random header noise: never crashes; whatever is accepted is self-consistent.
  dsptest::Noise nz(99);
  for (int it = 0; it < 4000; ++it) {
    std::vector<unsigned char> c(blob);
    const int flips = 1 + it % 4;
    for (int f = 0; f < flips; ++f) {
      const std::uint32_t at = static_cast<std::uint32_t>((nz.Next(1.0f) + 1.0f) * 15.99f);
      c[at] = static_cast<unsigned char>((nz.Next(1.0f) + 1.0f) * 127.9f);
    }
    if (ConvolutionParseIrBlob(c.data(), c.size(), &bi)) {
      SND_CHECK(32ull + 4ull * bi.channels * bi.frames == c.size());
      SND_CHECK(bi.channels >= 1u && bi.channels <= 16u && bi.sampleRate >= 1u && bi.sampleRate <= 384000u &&
                std::isfinite(bi.gain));
      const std::size_t nb = ConvolutionIrBytesFromBlob(c.data(), c.size(), kRate, 64);
      if (nb > 0u && nb < (64u << 20)) {
        dsptest::AlignedMem mm(nb, dsptest::kDirty);
        ConvolutionPrepareIrFromBlob(mm.get(), nb, c.data(), c.size(), kRate, 64);
      }
    }
  }
  // IR size limits: frames above 2^21 at the target rate are refused.
  SND_CHECK_EQ(ConvolutionIrTargetFrames(kConvolutionMaxIrFrames, kRate, kRate), kConvolutionMaxIrFrames);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(kConvolutionMaxIrFrames + 1u, kRate, kRate), 0u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(2000000u, 44100, 48000), 0u);  // 2176871 > 2^21
  SND_CHECK_EQ(ConvolutionIrTargetFrames(441, 44100, 48000), 480u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(1, 44100, 48000), 2u);  // ceil(1.088)
  SND_CHECK_EQ(ConvolutionIrTargetFrames(1000, 96000, 48000), 500u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(1001, 96000, 48000), 501u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(10, 0, 48000), 10u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(0, 48000, 48000), 0u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(10, 48000, 0), 0u);
  SND_CHECK_EQ(ConvolutionIrTargetFrames(10, 384001, 48000), 0u);
  // Invalid descs for ConvolutionIrBytes / PrepareIr.
  ConvolutionIrDesc bad = d;
  bad.channelCount = 0;
  SND_CHECK_EQ(ConvolutionIrBytes(bad, kRate, 64), 0u);
  bad = d;
  bad.frames = 0;
  SND_CHECK_EQ(ConvolutionIrBytes(bad, kRate, 64), 0u);
  for (std::uint32_t part : {0u, 32u, 63u, 65u, 100u, 8192u}) SND_CHECK_EQ(ConvolutionIrBytes(d, kRate, part), 0u);
  bad = d;
  bad.gain = std::nanf("");
  SND_CHECK(ConvolutionPrepareIr(m2.get(), irb, bad, kRate, 64) == nullptr);
  const float* withNull[2] = {h[0].data(), nullptr};
  bad = d;
  bad.channels = withNull;
  SND_CHECK(ConvolutionPrepareIr(m2.get(), irb, bad, kRate, 64) == nullptr);
  SND_CHECK(ConvolutionPrepareIr(nullptr, irb, d, kRate, 64) == nullptr);
}

// D-213 rate conversion. An impulse at source frame 441 (44.1 kHz) lands on
// frame 480 at 48 kHz with value 1; a band-limited IR (Gaussian-windowed tones
// at 1 / 5 / 12 kHz) prepared at 44.1, 32 and 96 kHz matches the same IR
// sampled at 48 kHz; downsampling 96 -> 48 kHz removes a 30 kHz component.
SND_TEST(IndepConvolution, IrRateConversion) {
  {
    std::vector<float> h(900, 0.0f);
    h[441] = 1.0f;
    Ir ir({h}, 64, 44100);
    SND_REQUIRE(ir.slot != 0u);
    SND_CHECK_EQ(ConvolutionIrFrames(ir.ir), 980u);  // ceil(900 * 48000 / 44100)
    Opt o;
    o.partition = 64;
    Conv fx(1, ir.slot, o);
    std::vector<float> x(1200, 0.0f);
    x[0] = 1.0f;
    const Multi y = Run(fx, {x}, {256});
    std::size_t at = 0;
    float best = 0.0f;
    for (std::size_t i = 0; i < y[0].size(); ++i)
      if (std::fabs(y[0][i]) > best) {
        best = std::fabs(y[0][i]);
        at = i;
      }
    SND_CHECK_EQ(at, 480u);
    SND_CHECK_NEAR(best, 1.0, 1e-4);
  }
  const auto ana = [](double t) {  // t in seconds
    const double env = std::exp(-0.5 * std::pow((t - 0.01) / 0.002, 2.0));
    return env * (0.3 * std::sin(2 * kPi * 1000 * t) + 0.2 * std::sin(2 * kPi * 5000 * t) +
                  0.1 * std::sin(2 * kPi * 12000 * t));
  };
  for (std::uint32_t src : {44100u, 32000u, 96000u}) {
    const std::uint32_t n = src / 50u;  // 20 ms
    std::vector<float> h(n);
    for (std::uint32_t i = 0; i < n; ++i) h[i] = static_cast<float>(ana(static_cast<double>(i) / src));
    Ir ir({h}, 64, src);
    SND_REQUIRE(ir.slot != 0u);
    Opt o;
    o.partition = 64;
    Conv fx(1, ir.slot, o);
    std::vector<float> x(1200, 0.0f);
    x[0] = 1.0f;
    const Multi y = Run(fx, {x}, {256});
    double e = 0.0;
    for (std::uint32_t i = 100; i < 900; ++i) e = std::max(e, std::fabs(y[0][i] - ana(static_cast<double>(i) / kRate)));
    snd::test::Note("IR %u -> 48000: max error %.3g (peak 0.6)", src, e);
    SND_CHECK_MSG(e < 2e-3, "IR %u Hz: error %g", src, e);
  }
  {
    // 96 kHz IR: 30 kHz + 6 kHz tones; at 48 kHz the 30 kHz part is gone (> 40 dB down).
    const std::uint32_t n = 9600;
    std::vector<float> h(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / 96000.0;
      h[i] = static_cast<float>(0.4 * std::sin(2 * kPi * 30000 * t) + 0.4 * std::sin(2 * kPi * 6000 * t));
    }
    Ir ir({h}, 256, 96000);
    Opt o;
    o.partition = 256;
    Conv fx(1, ir.slot, o);
    std::vector<float> x(6000, 0.0f);
    x[0] = 1.0f;
    const Multi y = Run(fx, {x}, {256});
    const double a6 = dsptest::ToneAmplitude(y[0].data() + 1000, 2400, 6000.0, kRate);
    const double a18 = dsptest::ToneAmplitude(y[0].data() + 1000, 2400, 18000.0, kRate);  // 48k - 30k alias
    snd::test::Note("96k -> 48k: 6 kHz %.4f, alias of 30 kHz at 18 kHz %.6f", a6, a18);
    SND_CHECK_NEAR(a6, 0.4, 0.01);
    SND_CHECK(a18 < 0.004);
  }
}

// C9: NaN / Inf input. The call outputs no non-finite sample; the next call is
// normal again and equals a fresh instance from that point; EffectReset equals
// a fresh EffectInit; dirty pool memory gives the same bits as clean memory.
SND_TEST(IndepConvolution, NanRecoveryResetAndDirtyMemory) {
  const std::vector<float> h = DecayIr(2000, 3, 400.0);
  Ir ir({h}, 128);
  Opt o;
  o.partition = 128;
  o.dry = 0.25f;
  o.maxIrMs = 100.0f;
  const std::vector<float> x = Noise(6000, 4);
  for (float bad : {std::nanf(""), INFINITY}) {
    Conv fx(2, ir.slot, o), fresh(2, ir.slot, o);
    Run(fx, {x, x}, {256});
    Multi nanIn(2, std::vector<float>(256, 0.1f));
    nanIn[1][77] = bad;
    const Multi yn = Run(fx, nanIn, {256});
    bool finite = true;
    for (auto& c : yn)
      for (float v : c) finite = finite && std::isfinite(v);
    SND_CHECK(finite);
    const Multi a = Run(fx, {x, x}, {256});
    const Multi b = Run(fresh, {x, x}, {256});
    SND_CHECK_MSG(a == b, "after a non-finite input the state is not a fresh one");
    SND_CHECK(fx.GuardIntact());
  }
  {
    Conv fx(1, ir.slot, o), fresh(1, ir.slot, o);
    Run(fx, {x}, {100});
    EffectReset(fx.st());
    SND_CHECK(Run(fx, {x}, {256}) == Run(fresh, {x}, {256}));
  }
  {
    Conv dirty(2, ir.slot, o, 0xFF), clean(2, ir.slot, o, 0x00), odd(2, ir.slot, o, 0x7F);
    const Multi a = Run(dirty, {x, x}, {256}), b = Run(clean, {x, x}, {256}), c = Run(odd, {x, x}, {256});
    SND_CHECK(a == b && b == c);
  }
  for (float bad : {std::nanf(""), INFINITY, -INFINITY}) {
    Multi taps{h, h};
    taps[1].back() = bad;
    Ir invalid(taps, 128);
    SND_CHECK(invalid.ir == nullptr);
  }
}

// Higher runtime rate than the sizing rate (D-214, existing rule): the
// capacity shrinks, nothing is written past the state, the output for an IR
// that fits is still the convolution.
SND_TEST(IndepConvolution, HigherRuntimeRateShrinksCapacity) {
  const std::vector<float> h = DecayIr(3000, 5, 600.0);
  Ir ir96({h}, 64, 96000, 1.0f, 96000);
  Opt sizing;
  sizing.partition = 64;
  sizing.maxIrMs = 100.0f;
  sizing.rate = kRate;
  Conv at48(1, 0, sizing);
  Opt o = sizing;
  o.rate = 96000;
  o.bytes = at48.bytes;  // state sized at 48 kHz, run at 96 kHz
  Conv fx(1, ir96.slot, o);
  SND_REQUIRE(fx.ok);
  const std::uint32_t capFrames = ConvolutionStateCapacityFrames(fx.st());
  snd::test::Note("capacity at 48 kHz %u, at 96 kHz with the same bytes %u frames",
                  ConvolutionStateCapacityFrames(at48.st()), capFrames);
  SND_CHECK(capFrames <= ConvolutionStateCapacityFrames(at48.st()) && capFrames > 0u);
  const std::vector<float> x = Noise(8000, 6);
  const Multi y = Run(fx, {x}, {256});
  SND_CHECK(fx.GuardIntact());
  const std::uint32_t used = ConvolutionStateActiveIrFrames(fx.st());
  SND_CHECK(used >= std::min<std::uint32_t>(3000u, capFrames) && used <= 3000u);
  std::vector<float> hc(h.begin(), h.begin() + used);
  SND_CHECK(MaxErr(y[0], Direct(x, hc)) < Tol(hc, 0.5f));
}

// MaxIrMs shorter than the IR: the IR is cut. The used length is reported and
// the output is the convolution with the cut IR.
SND_TEST(IndepConvolution, CapacityCutsLongIrs) {
  const std::vector<float> h = DecayIr(5000, 8, 3000.0);
  Ir ir({h}, 64);
  for (float zl : {1.0f, 0.0f}) {
    Opt o;
    o.partition = 64;
    o.zl = zl;
    o.maxIrMs = 10.0f;  // 480 frames -> cap 8 partitions = 512 frames
    Conv fx(1, ir.slot, o);
    const std::vector<float> x = Noise(4000, 3);
    const Multi y = Run(fx, {x}, {256});
    const std::uint32_t used = ConvolutionStateActiveIrFrames(fx.st());
    snd::test::Note("MaxIrMs 10, zl %g: capacity %u frames, IR frames used %u", static_cast<double>(zl),
                    ConvolutionStateCapacityFrames(fx.st()), used);
    SND_CHECK_EQ(used, (8u + (zl == 1.0f ? 1u : 0u)) * 64u);
    SND_CHECK_EQ(used, ConvolutionStateCapacityFrames(fx.st()));
    std::vector<float> hc(h.begin(), h.begin() + used);
    SND_CHECK(MaxErr(y[0], Direct(x, hc, zl != 0.0f ? 0u : 64u)) < Tol(hc, 0.5f));
  }
}

// C16: EffectProcessBatch and ProcessEffectChain give the same bits as EffectProcess.
SND_TEST(IndepConvolution, BatchAndChain) {
  const std::vector<float> h = DecayIr(1500, 9, 300.0);
  Ir ir({h}, 256);
  Opt o;
  o.dry = 0.4f;
  Conv a1(2, ir.slot, o), a2(2, ir.slot, o), a3(2, ir.slot, o);
  Conv b1(2, ir.slot, o), b2(2, ir.slot, o), b3(2, ir.slot, o);
  Conv c1(2, ir.slot, o);
  const Multi x1 = {Noise(256, 1), Noise(256, 2)}, x2 = {Noise(256, 3), Noise(256, 4)}, x3 = {Noise(256, 5), Noise(256, 6)};
  for (int rep = 0; rep < 20; ++rep) {
    Multi s1 = x1, s2 = x2, s3 = x3, t1 = x1, t2 = x2, t3 = x3, u1 = x1;
    float* p1[2] = {s1[0].data(), s1[1].data()};
    float* p2[2] = {s2[0].data(), s2[1].data()};
    float* p3[2] = {s3[0].data(), s3[1].data()};
    EffectProcess(a1.st(), p1, p1, 2, 256, nullptr, nullptr);
    EffectProcess(a2.st(), p2, p2, 2, 256, nullptr, nullptr);
    EffectProcess(a3.st(), p3, p3, 2, 256, nullptr, nullptr);
    float* q1[2] = {t1[0].data(), t1[1].data()};
    float* q2[2] = {t2[0].data(), t2[1].data()};
    float* q3[2] = {t3[0].data(), t3[1].data()};
    const EffectBatchItem items[3] = {{b1.st(), q1, q1, 2, 256, nullptr, nullptr},
                                      {b2.st(), q2, q2, 2, 256, nullptr, nullptr},
                                      {b3.st(), q3, q3, 2, 256, nullptr, nullptr}};
    {
      SND_RT_SCOPE();
      EffectProcessBatch(EffectKind::Convolution, items, 3);
    }
    SND_CHECK(s1 == t1 && s2 == t2 && s3 == t3);
    float* r1[2] = {u1[0].data(), u1[1].data()};
    const EffectSlot slot{kEffectConvolution, nullptr, c1.st(), nullptr, nullptr, 0u, 0u};
    {
      SND_RT_SCOPE();
      ProcessEffectChain(&slot, 1, r1, 2, 256);
    }
    SND_CHECK(u1 == s1);
  }
}

// Cost (report): 2 s stereo IR at 48 kHz, block 256 (B = 256) and 512, both
// latency modes: mean / worst per block and the peak ratio; EffectReset time.
SND_TEST(IndepConvolution, CostAndResetTime) {
  const std::uint32_t taps = 96000;
  const Multi h = {DecayIr(taps, 1, 20000.0), DecayIr(taps, 2, 20000.0)};
  for (std::uint32_t block : {256u, 512u}) {
    Ir ir(h, block);
    for (float zl : {1.0f, 0.0f}) {
      Opt o;
      o.maxFrames = block;
      o.zl = zl;
      o.dry = 0.5f;
      Conv fx(2, ir.slot, o);
      Multi buf = {Noise(block, 1), Noise(block, 2)};
      float* p[2] = {buf[0].data(), buf[1].data()};
      dsptest::Noise nz(1);
      double sum = 0.0, worst = 0.0;
      const int blocks = 1500;
      for (int b = 0; b < blocks; ++b) {
        for (auto& c : buf)
          for (float& v : c) v = nz.Next(0.5f);
        const auto t0 = std::chrono::steady_clock::now();
        EffectProcess(fx.st(), p, p, 2, block, nullptr, nullptr);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (b >= 400) {  // after the FDL is full
          sum += us;
          worst = std::max(worst, us);
        }
      }
      const double mean = sum / (blocks - 400);
      double rsum = 0.0, rworst = 0.0;
      for (int r = 0; r < 20; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        EffectReset(fx.st());
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        rsum += us;
        rworst = std::max(rworst, us);
      }
      snd::test::Note("convolution 2 s stereo, block %u, zl %g: mean %.1f us, worst %.1f us (peak/mean %.1f), "
                      "budget %.0f us; EffectReset of %llu bytes: mean %.1f us, worst %.1f us",
                      block, static_cast<double>(zl), mean, worst, worst / mean, block * 1e6 / kRate,
                      static_cast<unsigned long long>(fx.bytes),
                      rsum / 20.0, rworst);
    }
  }
}

namespace {
void Select(Conv& fx, float* data, std::uint32_t frames, std::uint32_t start, std::uint32_t end) {
  const float s = static_cast<float>(start), e = static_cast<float>(end);
  snd::ParamBlock params{&s, &e, 1, 1, nullptr};
  float* channels[] = {data};
  EffectProcess(fx.st(), channels, channels, 1, frames, &params, nullptr);
}
}

SND_TEST(IndepConvolution, LongCallUsesOnlyEndNumber) {
  Ir a({{0.5f}}, 64), middle({{8.0f}}, 64), end({{0.0f, 0.25f}}, 64);
  SND_REQUIRE(middle.slot == a.slot + 1u && end.slot == middle.slot + 1u);
  Opt o;
  o.partition = 64;
  Conv fx(1, a.slot, o);
  auto x = Noise(1024, 4), y = x;
  Select(fx, y.data(), 1024, a.slot, end.slot);
  SND_CHECK_EQ(y[0], 0.0f);  // dormant instance switches immediately
  for (unsigned i = 1; i < y.size(); ++i) SND_CHECK_EQ(y[i], 0.25f * x[i - 1]);
}

SND_TEST(IndepConvolution, IrSwitchCrossfadeMatchesDirectConvolution) {
  constexpr unsigned b = 64;
  const auto h1 = DecayIr(600, 1, 150), h2 = DecayIr(600, 2, 150), x = Noise(2048, 4);
  Ir a({h1}, b), c({h2}, b);
  SND_REQUIRE(a.slot && c.slot);
  for (float zl : {0.0f, 1.0f}) {
    Opt o;
    o.partition = b;
    o.zl = zl;
    const auto old = Direct(x, h1, zl == 0 ? b : 0), fresh = Direct(x, h2, zl == 0 ? b : 0);
    for (unsigned f : {0u, b / 2, b - 8, b - 1}) {
      const unsigned start = 1024 + f;
      auto y = x;
      Conv fx(1, a.slot, o);
      Select(fx, y.data(), start, a.slot, a.slot);
      Select(fx, y.data() + start, 2048 - start, a.slot, c.slot);
      for (unsigned i = 0; i < y.size(); ++i) {
        const double g = i < start ? 0.0 : std::min(1.0, double(i - start + 1) / b);
        const double want = (1.0 - g) * old[i] + g * fresh[i];
        SND_CHECK_NEAR(y[i], want, std::max(Tol(h1, 0.5f), Tol(h2, 0.5f)));
      }
    }
  }
}

SND_TEST(IndepConvolution, SwitchStepMeasurements) {
  constexpr unsigned b = 64;
  Ir a({{1.0f}}, b), c({{-1.0f}}, b);
  SND_REQUIRE(a.slot && c.slot);
  Opt o;
  o.partition = b;
  for (unsigned f : {0u, b / 2, b - 8, b - 1}) {
    const unsigned start = 256 + f;
    Conv fx(1, a.slot, o);
    std::vector<float> y(512, 1.0f);
    Select(fx, y.data(), start, a.slot, a.slot);
    Select(fx, y.data() + start, 512 - start, a.slot, c.slot);
    float step = 0;
    for (unsigned i = start; i < y.size(); ++i) step = std::max(step, std::fabs(y[i] - y[i - 1]));
    SND_CHECK_EQ(step, 2.0f / b);
    snd::test::Note("DC 1, gains +1 to -1, B=%u f=%u: maximum adjacent step %.8f", b, f, step);
  }
}

SND_TEST(IndepConvolution, FadeOutWithRetiredMemoryAndFadeInFromSilence) {
  constexpr unsigned b = 64;
  auto h = DecayIr(315, 11, 80);
  Ir a({h}, b), copy({h}, b);
  SND_REQUIRE(a.slot && copy.slot);
  Opt o;
  o.partition = b;
  Conv fx(1, a.slot, o);
  auto x = Noise(1024, 9), y = x;
  const auto reference = Direct(x, h);
  Select(fx, y.data(), 512, a.slot, a.slot);
  const auto retired = a.slot;
  SND_REQUIRE(ConvolutionUnregisterIr(retired));
  a.slot = 0;
  // No concurrent audio call. Retire the backing storage before rendering again.
  std::memset(a.mem.get(), 0xff, a.mem.size());
  Select(fx, y.data() + 512, 256, retired, retired);
  Select(fx, y.data() + 768, 256, 0, copy.slot);
  for (unsigned i = 512; i < 1024; ++i) {
    const double g = i < 768 ? std::max(0.0, 1.0 - double(i - 511) / b)
                             : std::min(1.0, double(i - 767) / b);
    SND_CHECK_NEAR(y[i], g * reference[i], Tol(h, 0.5f));
  }
}

SND_TEST(IndepConvolution, SubsequentSwitchAtFadeBoundary) {
  constexpr unsigned b = 64;
  Ir one({{1.0f}}, b), zero({{0.0f}}, b), neg({{-1.0f}}, b);
  SND_REQUIRE(one.slot && zero.slot && neg.slot);
  Opt o;
  o.partition = b;
  Conv fx(1, one.slot, o);
  std::vector<float> y(256, 1.0f);
  Select(fx, y.data(), 64, one.slot, one.slot);
  Select(fx, y.data() + 64, 8, one.slot, zero.slot);
  Select(fx, y.data() + 72, 56, zero.slot, zero.slot);
  Select(fx, y.data() + 128, 128, zero.slot, neg.slot);
  for (unsigned i = 64; i < 128; ++i) SND_CHECK_EQ(y[i], 1.0f - float(i - 63) / b);
  for (unsigned i = 128; i < 192; ++i) SND_CHECK_EQ(y[i], -float(i - 127) / b);
  for (unsigned i = 192; i < 256; ++i) SND_CHECK_EQ(y[i], -1.0f);
}

namespace {
struct Registrations {
  std::vector<unsigned> numbers;
  ~Registrations() {
    for (auto id : numbers) ConvolutionUnregisterIr(id);
  }
};
}

SND_TEST(IndepConvolution, RegistryFindsSoleFreeEntryAtNumberWrap) {
  Ir delta({{1.0f}}, 64, kRate, 1.0f, kRate, false);
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


SND_TEST(IndepConvolution, FadeLastsFullPartitionAfterUnregister) {
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

SND_TEST(IndepConvolution, PendingSwitchStartsAtFirstFreeSample) {
  constexpr unsigned b = 64, first = 256 + 13;
  const auto h1 = DecayIr(340, 91, 140), h2 = DecayIr(410, 73, 190), h3 = DecayIr(480, 21, 250);
  Ir a({h1}, b), superseded({{6.0f}}, b), current({h2}, b), last({h3}, b);
  SND_REQUIRE(a.slot && current.slot && superseded.slot && last.slot);
  const auto x = Noise(640, 58);
  for (auto delay : {0u, b}) {
    const auto old = Direct(x, h1, delay), middle = Direct(x, h2, delay), fresh = Direct(x, h3, delay);
    std::vector<float> previous;
    for (auto maxFrames : {8u, 13u, 256u}) {
      Opt o;
      o.partition = b;
      o.zl = delay == 0 ? 1.0f : 0.0f;
      o.maxFrames = maxFrames;
      Conv fx(1, a.slot, o);
      auto y = x;
      Select(fx, y.data(), first, a.slot, a.slot);
      Select(fx, y.data() + first, 9, a.slot, current.slot);
      Select(fx, y.data() + first + 9, 17, current.slot, superseded.slot);
      Select(fx, y.data() + first + 26, unsigned(y.size()) - first - 26, superseded.slot, last.slot);
      for (unsigned i = 0; i < y.size(); ++i) {
        const double u = i < first ? 0 : std::min(1.0, double(i - first + 1) / b);
        const double v = i < first + b ? 0 : std::min(1.0, double(i - first - b + 1) / b);
        const double want = (1 - v) * ((1 - u) * old[i] + u * middle[i]) + v * fresh[i];
        SND_CHECK_NEAR(y[i], want, std::max({Tol(h1, 0.5f), Tol(h2, 0.5f), Tol(h3, 0.5f)}));
      }
      if (!previous.empty()) SND_CHECK(previous == y);
      previous = y;
    }
  }
}
