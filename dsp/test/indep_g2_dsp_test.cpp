// Independent tests of stage 3 group 2 in dsp (no core): the ParamEq band type 8 (allpass,
// D-135) and non-finite / out-of-range parameters through ParamBlock ramps for every
// standard kind on 1, 2 and 6 channels. Also built against the scalar SIMD path.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "snd/dsp/effects.hpp"
#include "snd/test/test.hpp"

namespace {

// 64-byte aligned memory (std::aligned_alloc is not in the MinGW C library).
inline void* AlignedAlloc64(std::size_t bytes) {
  void* raw = std::malloc(bytes + 64u + sizeof(void*));
  if (raw == nullptr) return nullptr;
  const std::uintptr_t a = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + 63u) & ~std::uintptr_t{63u};
  reinterpret_cast<void**>(a)[-1] = raw;
  return reinterpret_cast<void*>(a);
}
inline void AlignedFree64(void* p) {
  if (p != nullptr) std::free(static_cast<void**>(p)[-1]);
}

namespace dsp = snd::dsp;
constexpr std::uint32_t kRate = 48000;

struct Inst {
  explicit Inst(snd::Id type, std::uint32_t channels, std::vector<float> params) : p(std::move(params)) {
    d = snd::EffectDesc{type, channels, 256, static_cast<std::uint32_t>(p.size()), p.empty() ? nullptr : p.data(),
                        nullptr, 0, kRate, 0};
    bytes = dsp::EffectStateBytes(d, kRate);
    mem = bytes != 0u ? AlignedAlloc64(bytes) : nullptr;
    ok = mem != nullptr && dsp::EffectInit(mem, bytes, d, kRate);
  }
  ~Inst() { AlignedFree64(mem); }
  Inst(const Inst&) = delete;
  Inst& operator=(const Inst&) = delete;
  std::vector<float> p;
  snd::EffectDesc d{};
  std::uint32_t bytes = 0;
  void* mem = nullptr;
  bool ok = false;
};

// Runs a mono sine of `hz` through band 0 = allpass at `f0` and returns (gain, phase in deg)
// of the steady state.
void AllpassResponse(float f0, float q, double hz, double* gain, double* phaseDeg) {
  Inst e(dsp::kEffectParamEq, 1, {8.0f, f0, q, 0.0f, 0.0f, 500.0f, 0.707f, 0.0f, 0.0f, 2000.0f, 0.707f, 0.0f, 0.0f,
                                   8000.0f, 0.707f, 0.0f});
  if (!e.ok) {
    *gain = -1;
    return;
  }
  const std::size_t n = 48128;  // 188 blocks
  std::vector<float> x(n), y(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(std::sin(6.283185307179586 * hz * double(i) / kRate));
  for (std::size_t at = 0; at < n; at += 256) {
    const float* in[1] = {x.data() + at};
    float* out[1] = {y.data() + at};
    dsp::EffectProcess(e.mem, in, out, 1, 256, nullptr);
  }
  // Steady state (last 0.5 s): quadrature correlation.
  double xi = 0, xq = 0, yi = 0, yq = 0;
  const double w = 6.283185307179586 * hz / kRate;
  for (std::size_t i = n / 2; i < n; ++i) {
    const double c = std::cos(w * double(i)), s = std::sin(w * double(i));
    xi += x[i] * s;
    xq += x[i] * c;
    yi += y[i] * s;
    yq += y[i] * c;
  }
  *gain = std::sqrt(yi * yi + yq * yq) / std::sqrt(xi * xi + xq * xq);
  double ph = (std::atan2(yq, yi) - std::atan2(xq, xi)) * 180.0 / 3.141592653589793;
  while (ph > 0.0) ph -= 360.0;
  while (ph <= -360.0) ph += 360.0;
  *phaseDeg = ph;
}

}  // namespace

// D-135: band type 8 is an allpass: gain 1 everywhere, -180 deg at its frequency
// (second-order allpass: 0 deg at DC, -360 deg at Nyquist).
SND_TEST(IndepG2Dsp, ParamEqAllpassBand) {
  for (const double hz : {60.0, 300.0, 1500.0, 6000.0, 15000.0}) {
    double g = 0, ph = 0;
    AllpassResponse(1500.0f, 0.9f, hz, &g, &ph);
    SND_CHECK_NEAR(g, 1.0, 2e-3);
  }
  double g = 0, ph = 0;
  AllpassResponse(1500.0f, 0.9f, 1500.0, &g, &ph);
  SND_CHECK_NEAR(ph, -180.0, 3.0);
  AllpassResponse(1500.0f, 0.9f, 60.0, &g, &ph);
  SND_CHECK(ph > -40.0);  // near 0 deg well below the frequency
  AllpassResponse(1500.0f, 0.9f, 15000.0, &g, &ph);
  SND_CHECK(ph < -320.0);  // near -360 deg well above
  // The band gain does not apply to an allpass.
  Inst e(dsp::kEffectParamEq, 1, {8.0f, 1000.0f, 1.0f, 24.0f});
  SND_REQUIRE(e.ok);
  std::vector<float> x(4096), y(4096);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(std::sin(6.283185307179586 * 1000.0 * double(i) / kRate));
  for (std::size_t at = 0; at < x.size(); at += 256) {
    const float* in[1] = {x.data() + at};
    float* out[1] = {y.data() + at};
    dsp::EffectProcess(e.mem, in, out, 1, 256, nullptr);
  }
  float peak = 0;
  for (std::size_t i = 2048; i < y.size(); ++i) peak = std::fmax(peak, std::fabs(y[i]));
  SND_CHECK_NEAR(peak, 1.0, 0.01);
}

// Every standard kind, 1 / 2 / 6 channels, ParamBlock ramps whose start / end are NaN, +-Inf,
// +-1e30 and the range limits: the output stays finite and bounded, and the next normal call
// is normal (effects.hpp: NaN -> default, Inf -> the bound).
SND_TEST(IndepG2Dsp, NonFiniteAndOutOfRangeRampsStayFinite) {
  const dsp::EffectKind kinds[] = {
      dsp::EffectKind::Reverb,     dsp::EffectKind::Echo,       dsp::EffectKind::Delay,      dsp::EffectKind::Chorus,
      dsp::EffectKind::Flanger,    dsp::EffectKind::Distortion, dsp::EffectKind::Compressor, dsp::EffectKind::Limiter,
      dsp::EffectKind::ParamEq,    dsp::EffectKind::Lowpass,    dsp::EffectKind::Highpass,   dsp::EffectKind::PitchShifter,
      dsp::EffectKind::Normalize,  dsp::EffectKind::Surround};
  const float nan = std::nanf("");
  const float values[] = {nan, INFINITY, -INFINITY, 1e30f, -1e30f, 0.0f};
  for (const std::uint32_t ch : {1u, 2u, 6u}) {
    for (const dsp::EffectKind k : kinds) {
      const dsp::EffectInfo& info = dsp::GetEffectInfo(k);
      Inst e(info.typeId, ch, {});
      SND_REQUIRE(e.ok);
      std::vector<std::vector<float>> buf(ch, std::vector<float>(256));
      std::vector<float*> ptr(ch);
      for (std::uint32_t c = 0; c < ch; ++c) ptr[c] = buf[c].data();
      std::vector<float> start(info.paramCount), end(info.paramCount);
      std::uint64_t n = 0;
      bool finite = true;
      float peak = 0;
      for (const float a : values) {
        for (const float b : values) {
          for (std::uint32_t i = 0; i < info.paramCount; ++i) {
            start[i] = a;
            end[i] = b;
          }
          snd::ParamBlock pb{start.data(), end.data(), info.paramCount, 0xFFFFFFFFu, nullptr};
          for (std::uint32_t c = 0; c < ch; ++c) {
            for (std::uint32_t i = 0; i < 256; ++i) {
              buf[c][i] = 0.8f * static_cast<float>(std::sin(0.05 * double(n + i) * (1.0 + c)));
            }
          }
          n += 256;
          dsp::EffectProcess(e.mem, ptr.data(), ptr.data(), ch, 256, &pb);
          for (std::uint32_t c = 0; c < ch; ++c) {
            for (float v : buf[c]) {
              finite = finite && std::isfinite(v);
              peak = std::fmax(peak, std::fabs(v));
            }
          }
          for (std::uint32_t i = 0; i < info.paramCount; ++i) {
            SND_CHECK(std::isfinite(dsp::EffectParamValue(e.mem, i)));
          }
        }
      }
      SND_CHECK_MSG(finite && peak < 1000.0f, "%s, %u channels: finite %d, peak %g", info.name, ch, finite ? 1 : 0, peak);
    }
  }
}

// effects.hpp: Echo "Delay is clamped to MaxDelay", Delay "Time is clamped to MaxDelay". A
// value above MaxDelay gives the same delay as MaxDelay itself (480 frames for 10 ms). Was
// Defect_DelayAboveMaxDelayIsOneFrameLongerThanMaxDelay (481), fixed in b527335.
SND_TEST(IndepG2Dsp, DelayAboveMaxDelayEqualsMaxDelay) {
  auto onset = [](snd::Id kind, std::vector<float> params) {
    Inst e(kind, 1, std::move(params));
    if (!e.ok) return -1;
    std::vector<float> x(256);
    for (int k = 0; k < 8; ++k) {
      std::fill(x.begin(), x.end(), 0.0f);
      if (k == 0) x[0] = 1.0f;
      float* io[1] = {x.data()};
      dsp::EffectProcess(e.mem, io, io, 1, 256, nullptr);
      for (int i = (k == 0 ? 1 : 0); i < 256; ++i) {
        if (std::fabs(x[static_cast<std::size_t>(i)]) > 0.1f) return k * 256 + i;
      }
    }
    return -1;
  };
  const int echoAt = onset(dsp::kEffectEcho, {10.0f, 0.0f, 0.0f, 1.0f, 10.0f});
  const int echoAbove = onset(dsp::kEffectEcho, {100.0f, 0.0f, 0.0f, 1.0f, 10.0f});
  const int delayAt = onset(dsp::kEffectDelay, {10.0f, 0.0f, -96.0f, 0.0f, 20000.0f, 0.0f, 10.0f});
  const int delayAbove = onset(dsp::kEffectDelay, {100.0f, 0.0f, -96.0f, 0.0f, 20000.0f, 0.0f, 10.0f});
  std::printf("  echo: at MaxDelay %d, above %d | delay: at MaxDelay %d, above %d\n", echoAt, echoAbove, delayAt,
              delayAbove);
  SND_CHECK_EQ(echoAt, 480);
  SND_CHECK_EQ(echoAbove, 480);
  SND_CHECK_EQ(delayAt, 480);
  SND_CHECK_EQ(delayAbove, 480);
}

// --- Re-check 2 (b527335: Echo / Delay clamp to the MaxDelay value) ---------------------------
namespace {

// An instance sized at `sizingRate` and run at `runRate` (desc.sample_rate 0: sizes and inits
// with their own rate arguments; a run rate above the sizing rate clamps to what fits).
struct RInst {
  RInst(snd::Id type, std::uint32_t channels, std::vector<float> params, std::uint32_t sizingRate, std::uint32_t runRate)
      : p(std::move(params)) {
    d = snd::EffectDesc{type, channels, 256, static_cast<std::uint32_t>(p.size()), p.data(), nullptr, 0, 0, 0};
    bytes = dsp::EffectStateBytes(d, sizingRate);
    mem = bytes != 0u ? AlignedAlloc64(bytes) : nullptr;
    ok = mem != nullptr && dsp::EffectInit(mem, bytes, d, runRate);
  }
  ~RInst() { AlignedFree64(mem); }
  RInst(const RInst&) = delete;
  RInst& operator=(const RInst&) = delete;
  std::vector<float> p;
  snd::EffectDesc d{};
  std::uint32_t bytes = 0;
  void* mem = nullptr;
  bool ok = false;
};

// Output of `blocks` blocks for an impulse at frame 0 on channel 0 (others silent) through e.
std::vector<std::vector<float>> ImpulseThrough(void* mem, std::uint32_t ch, std::uint32_t blocks,
                                               const snd::ParamBlock* pb = nullptr) {
  std::vector<std::vector<float>> out(ch, std::vector<float>(std::size_t{blocks} * 256, 0.0f));
  for (std::uint32_t k = 0; k < blocks; ++k) {
    std::vector<float*> io(ch);
    for (std::uint32_t c = 0; c < ch; ++c) {
      io[c] = out[c].data() + std::size_t{k} * 256;
      if (k == 0 && c == 0) io[c][0] = 1.0f;
    }
    dsp::EffectProcess(mem, io.data(), io.data(), ch, 256, pb);
  }
  return out;
}
int FirstAbove(const std::vector<float>& x, int from, float thr = 0.1f) {
  for (std::size_t i = static_cast<std::size_t>(from); i < x.size(); ++i) {
    if (std::fabs(x[i]) > thr) return static_cast<int>(i);
  }
  return -1;
}
std::vector<float> EchoP(float ms, float maxMs, float decay = 0.0f) { return {ms, decay, 0.0f, 1.0f, maxMs}; }
std::vector<float> DelayP(float ms, float maxMs, float fb = 0.0f, float pingPong = 0.0f) {
  return {ms, fb, -96.0f, 0.0f, 20000.0f, pingPong, maxMs};
}

}  // namespace

// At, just below and above MaxDelay, at run rates below / equal / above the sizing rate, 1 / 2 /
// 6 channels: above == at; at == MaxDelay frames when the ring holds it; below is shorter.
SND_TEST(IndepG2Dsp, ClampToMaxDelayAtAnyRateAndChannelCount) {
  struct Rates {
    std::uint32_t sizing, run;
  };
  const Rates rates[] = {{48000, 48000}, {48000, 24000}, {48000, 44100}, {24000, 48000}, {44100, 96000}};
  for (const Rates& r : rates) {
    for (const std::uint32_t ch : {1u, 2u, 6u}) {
      for (int kind = 0; kind < 2; ++kind) {
        const snd::Id id = kind == 0 ? dsp::kEffectEcho : dsp::kEffectDelay;
        auto onset = [&](float ms) {
          RInst e(id, ch, kind == 0 ? EchoP(ms, 10.0f) : DelayP(ms, 10.0f), r.sizing, r.run);
          if (!e.ok) return -2;
          const auto y = ImpulseThrough(e.mem, ch, 8);
          return FirstAbove(y[0], 1);
        };
        const int at = onset(10.0f), above = onset(37.0f), below = onset(9.0f);
        const int maxFrames = static_cast<int>(10.0 * r.run / 1000.0 + 0.5);
        const bool fitsRing = r.run <= r.sizing;
        SND_CHECK_MSG(above == at, "%s %u->%u Hz %u ch: at MaxDelay %d, above %d", kind == 0 ? "echo" : "delay", r.sizing,
                      r.run, ch, at, above);
        if (fitsRing) {
          SND_CHECK_MSG(std::abs(at - maxFrames) <= 1, "%s %u->%u Hz: at MaxDelay %d, expected %d",
                        kind == 0 ? "echo" : "delay", r.sizing, r.run, at, maxFrames);
        }
        // Below MaxDelay is shorter, unless the ring (sized at the lower sizing rate) bounds both.
        SND_CHECK_MSG(below > 0 && (fitsRing ? below < at : below <= at && at < maxFrames),
                      "%s %u->%u Hz %u ch: below %d, at %d (MaxDelay %d frames)", kind == 0 ? "echo" : "delay", r.sizing,
                      r.run, ch, below, at, maxFrames);
      }
    }
  }
}

// The delay ramps inside blocks across MaxDelay (5 ms -> 100 ms -> 3 ms, ParamBlock start/end):
// no jump past the clamp, no NaN, no read outside the ring (ASan), and after the ramp the delay is
// MaxDelay. With feedback 0.9 and ping-pong, on 2 and 6 channels.
SND_TEST(IndepG2Dsp, DelayRampAcrossMaxDelayStaysInsideTheRing) {
  for (const std::uint32_t ch : {2u, 6u}) {
    for (int kind = 0; kind < 2; ++kind) {
      const snd::Id id = kind == 0 ? dsp::kEffectEcho : dsp::kEffectDelay;
      std::vector<float> base = kind == 0 ? EchoP(5.0f, 10.0f, 0.9f) : DelayP(5.0f, 10.0f, 0.9f, 1.0f);
      RInst e(id, ch, base, 48000, 48000);
      SND_REQUIRE(e.ok);
      std::vector<float> st = base, en = base;
      const std::uint32_t n = static_cast<std::uint32_t>(base.size());
      std::vector<std::vector<float>> buf(ch, std::vector<float>(256));
      std::vector<float*> io(ch);
      bool finite = true;
      float peak = 0;
      std::uint64_t t = 0;
      const float path[] = {5.0f, 9.5f, 10.5f, 100.0f, 100.0f, 9.99f, 10.01f, 3.0f, 10.0f, 4000.0f, 1.0f};
      for (std::size_t k = 0; k + 1 < sizeof(path) / sizeof(path[0]); ++k) {
        for (int rep = 0; rep < 6; ++rep) {
          st[0] = rep == 0 ? path[k] : path[k + 1];
          en[0] = path[k + 1];
          snd::ParamBlock pb{st.data(), en.data(), n, 1u, nullptr};
          for (std::uint32_t c = 0; c < ch; ++c) {
            for (std::uint32_t i = 0; i < 256; ++i) buf[c][i] = 0.5f * static_cast<float>(std::sin(0.013 * double(t + i) * (c + 1)));
            io[c] = buf[c].data();
          }
          t += 256;
          dsp::EffectProcess(e.mem, io.data(), io.data(), ch, 256, &pb);
          for (std::uint32_t c = 0; c < ch; ++c) {
            for (float v : buf[c]) {
              finite = finite && std::isfinite(v);
              peak = std::fmax(peak, std::fabs(v));
            }
          }
        }
      }
      SND_CHECK_MSG(finite && peak < 20.0f, "%s %u ch: finite %d peak %g", kind == 0 ? "echo" : "delay", ch, finite ? 1 : 0,
                    peak);
      // Settle at 100 ms (clamped) with no feedback: the delay is MaxDelay (480 frames).
      std::vector<float> fin = kind == 0 ? EchoP(100.0f, 10.0f) : DelayP(100.0f, 10.0f);
      snd::ParamBlock hold{fin.data(), fin.data(), n, 1u, nullptr};
      for (int k = 0; k < 40; ++k) {
        for (std::uint32_t c = 0; c < ch; ++c) {
          std::fill(buf[c].begin(), buf[c].end(), 0.0f);
          io[c] = buf[c].data();
        }
        dsp::EffectProcess(e.mem, io.data(), io.data(), ch, 256, &hold);
      }
      const auto y = ImpulseThrough(e.mem, ch, 6, &hold);
      SND_CHECK_MSG(FirstAbove(y[0], 1) == 480 || (kind == 1 && FirstAbove(y[1], 1) == 480),
                    "%s %u ch: after the ramp the delay is %d (left) %d (right)", kind == 0 ? "echo" : "delay", ch,
                    FirstAbove(y[0], 1), FirstAbove(y[1], 1));
    }
  }
}

// The batch path (EffectProcessBatch, several instances of one kind) gives the same samples as
// one EffectProcess per instance, with delays at / above / below MaxDelay.
SND_TEST(IndepG2Dsp, BatchEqualsSingleAcrossTheClamp) {
  for (int kind = 0; kind < 2; ++kind) {
    const snd::Id id = kind == 0 ? dsp::kEffectEcho : dsp::kEffectDelay;
    const float ms[3] = {10.0f, 55.0f, 7.3f};
    auto params = [&](float m) { return kind == 0 ? EchoP(m, 10.0f, 0.5f) : DelayP(m, 10.0f, 0.5f, 1.0f); };
    std::vector<std::vector<float>> single(3), batch(3);
    for (int mode = 0; mode < 2; ++mode) {
      RInst a(id, 2, params(ms[0]), 48000, 48000), b(id, 2, params(ms[1]), 48000, 48000), c(id, 2, params(ms[2]), 48000, 48000);
      SND_REQUIRE(a.ok && b.ok && c.ok);
      RInst* inst[3] = {&a, &b, &c};
      std::vector<float> bufs[3][2];
      for (auto& x : bufs) {
        x[0].assign(256, 0.0f);
        x[1].assign(256, 0.0f);
      }
      auto& dst = mode == 0 ? single : batch;
      for (int k = 0; k < 10; ++k) {
        float* io[3][2];
        for (int i = 0; i < 3; ++i) {
          for (int c2 = 0; c2 < 2; ++c2) {
            std::fill(bufs[i][c2].begin(), bufs[i][c2].end(), 0.0f);
            if (k == 0 && c2 == 0) bufs[i][c2][3] = 1.0f;
            io[i][c2] = bufs[i][c2].data();
          }
        }
        if (mode == 0) {
          for (int i = 0; i < 3; ++i) dsp::EffectProcess(inst[i]->mem, io[i], io[i], 2, 256, nullptr);
        } else {
          dsp::EffectBatchItem items[3];
          for (int i = 0; i < 3; ++i) items[i] = dsp::EffectBatchItem{inst[i]->mem, io[i], io[i], 2, 256, nullptr, nullptr};
          dsp::EffectProcessBatch(kind == 0 ? dsp::EffectKind::Echo : dsp::EffectKind::Delay, items, 3);
        }
        for (int i = 0; i < 3; ++i) {
          dst[static_cast<std::size_t>(i)].insert(dst[static_cast<std::size_t>(i)].end(), bufs[i][0].begin(), bufs[i][0].end());
          dst[static_cast<std::size_t>(i)].insert(dst[static_cast<std::size_t>(i)].end(), bufs[i][1].begin(), bufs[i][1].end());
        }
      }
    }
    for (int i = 0; i < 3; ++i) SND_CHECK(single[static_cast<std::size_t>(i)] == batch[static_cast<std::size_t>(i)]);
    // Instance b (55 ms, above MaxDelay) repeats at 480 frames like instance a (10 ms).
    SND_CHECK_EQ(FirstAbove(batch[1], 4), FirstAbove(batch[0], 4));
  }
}
