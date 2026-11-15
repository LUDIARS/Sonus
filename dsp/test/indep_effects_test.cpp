// Independent effect tests, part 1: memory bounds, robustness with extreme /
// invalid parameters, identity settings, in-place, block-split invariance,
// tails, reset, batch / chain / plugins, pool sizing, RT safety.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "dsp_test_util.hpp"
#include "indep_fx_util.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/dsp/meter.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using namespace indepfx;
using dsptest::Audio;

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
}

// EffectStateBytes is exactly enough: no access outside [state, state + bytes)
// for 1..8 channels, 22.05..192 kHz, max_frames 1..1024, calls of 1 frame to
// 3 x max_frames, ramps over the full parameter ranges, sidechain, reset; and
// when the runtime rate is higher than the sizing rate (capacities clamped).
SND_TEST(EffectsIndep, StateStaysInsideStateBytes) {
  std::uint32_t bad = 0, runs = 0;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    const EffectInfo& info = GetEffectInfo(kind);
    for (std::uint32_t ch : {1u, 2u, 3u, 6u, 8u}) {
      for (std::uint32_t rate : {22050u, 44100u, 96000u, 192000u}) {
        for (std::uint32_t maxFrames : {1u, 37u, 256u}) {
          for (std::uint32_t runRate : {rate, rate * 2u}) {
            // Init-only parameters at their maxima (largest buffers).
            Fx fx(kind, ch, maxFrames, rate, {}, runRate, true);
            if (!fx.ok) {
              ++bad;
              SND_CHECK_MSG(false, "%s ch %u rate %u/%u max %u: init failed", Short(kind), ch, rate, runRate, maxFrames);
              continue;
            }
            Audio a(ch, 3 * maxFrames + 1), sc(2, 3 * maxFrames + 1);
            float s[kMaxEffectParams], e[kMaxEffectParams];
            std::uint64_t t = 0;
            for (std::uint32_t call = 0; call < 6; ++call) {
              const std::uint32_t n = call == 0 ? 1u : (call == 1 ? maxFrames : (call == 2 ? 3 * maxFrames : 1 + (call * 97) % (2 * maxFrames)));
              for (std::uint32_t p = 0; p < info.paramCount; ++p) {
                const EffectParamInfo& pi = info.params[p];
                s[p] = (call & 1) ? pi.minValue : pi.maxValue;
                e[p] = (call & 1) ? pi.maxValue : pi.minValue;
              }
              snd::ParamBlock pb{s, e, info.paramCount, 0xFFFFFFFFu, nullptr};
              Material(a, t, n, 1.0f, runRate);
              Material(sc, t + 5, n, 1.0f, runRate);
              EffectSidechain side{sc.cp(), 2};
              Run(fx, a.cp(), a.p(), ch, n, &pb, call % 3 == 1 ? &side : nullptr);
              t += n;
              if (HasNonFinite(a.ch(0), n) || HasNonFinite(a.ch(ch - 1), n)) {
                ++bad;
                SND_CHECK_MSG(false, "%s ch %u rate %u/%u max %u call %u: non-finite output", Short(kind), ch, rate, runRate,
                              maxFrames, call);
              }
            }
            EffectReset(fx.state());
            Material(a, t, maxFrames);
            Run(fx, a.cp(), a.p(), ch, maxFrames);
            (void)EffectIsTailDone(fx.state());
            ++runs;
            if (!fx.GuardsOk()) {
              ++bad;
              SND_CHECK_MSG(false, "%s ch %u rate %u/%u max_frames %u: wrote outside EffectStateBytes (%u bytes)", Short(kind),
                            ch, rate, runRate, maxFrames, fx.bytes);
            }
          }
        }
      }
    }
  }
  snd::test::Note("%u instances checked", runs);
  SND_CHECK_EQ(bad, 0u);
}

// Silence in -> exact silence out from a fresh instance, for the defaults and
// for every parameter at its min / max / beyond / NaN / +-Inf (via the bank
// values and via ParamBlock). Then loud noise: finite and bounded; then silence:
// the tail ends in finite time and is then exactly silent.
SND_TEST(EffectsIndep, ExtremeParametersSilenceFiniteTail) {
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    const EffectInfo& info = GetEffectInfo(kind);
    for (std::int32_t p = -1; p < static_cast<std::int32_t>(info.paramCount); ++p) {
      for (int variant = 0; variant < (p < 0 ? 1 : 7); ++variant) {
        float v = 0.0f;
        if (p >= 0) {
          const EffectParamInfo& pi = info.params[p];
          const float range = pi.maxValue - pi.minValue;
          const float vals[7] = {pi.minValue, pi.maxValue, pi.minValue - range - 1.0f, pi.maxValue + range + 1.0f, kNaN, INFINITY,
                                 -INFINITY};
          v = vals[variant];
        }
        const bool viaBlock = (variant % 2) == 1;  // half through ParamBlock, half through the bank values
        Fx fx = p >= 0 && !viaBlock ? Fx(kind, 2, 256, 48000, {{static_cast<std::uint32_t>(p), v}}) : Fx(kind, 2, 256, 48000);
        SND_REQUIRE(fx.ok);
        float pv[kMaxEffectParams];
        EffectDefaultParams(kind, pv);
        if (p >= 0 && viaBlock) pv[p] = v;
        snd::ParamBlock pb{pv, pv, info.paramCount, 0xFFFFFFFFu, nullptr};
        const snd::ParamBlock* pbp = viaBlock ? &pb : nullptr;
        Audio a(2, 256);
        // 1. Silence in.
        float out = 0.0f;
        for (int b = 0; b < 20; ++b) {
          a.Zero();
          Run(fx, a.cp(), a.p(), 2, 256, pbp);
          out = std::max(out, MaxAbs(a, 256));
        }
        // 2. Loud noise for 1 s.
        float loud = 0.0f;
        dsptest::Noise nz(7);
        for (int b = 0; b < 188; ++b) {
          for (std::uint32_t c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < 256; ++i) a.ch(c)[i] = nz.Next(1.0f);
          Run(fx, a.cp(), a.p(), 2, 256, pbp);
          loud = std::max(loud, MaxAbs(a, 256));
        }
        // 3. Silence until the tail ends (<= 60 s), then 1 s of exact silence.
        std::uint32_t blocks = 0;
        while (!EffectIsTailDone(fx.state()) && blocks < 48000u * 240u / 256u) {
          a.Zero();
          Run(fx, a.cp(), a.p(), 2, 256, pbp);
          loud = std::max(loud, MaxAbs(a, 256));
          ++blocks;
        }
        const bool tail = EffectIsTailDone(fx.state());
        float after = 0.0f;
        for (int b = 0; b < 188; ++b) {
          a.Zero();
          Run(fx, a.cp(), a.p(), 2, 256, pbp);
          after = std::max(after, MaxAbs(a, 256));
        }
        const char* pname = p >= 0 ? info.params[p].name : "(defaults)";
        const char* vname[7] = {"min", "max", "below min", "above max", "NaN", "+Inf", "-Inf"};
        // Bound: 100 x the input peak (+40 dB) covers every documented gain stage.
        // Echo Decay = 1 is a lossless loop: its tail never ends by definition
        // (spec gap, reported separately); only boundedness is required there.
        const bool lossless = kind == EffectKind::Echo && p == 1 && (viaBlock ? pv[1] : fx.params[1]) >= 1.0f;
        const bool okNow = out == 0.0f && std::isfinite(loud) && loud < 100.0f && (lossless || (tail && after == 0.0f));
        if (lossless) snd::test::Note("echo Decay=%s: tail done %d after %.0f s (lossless loop)", vname[variant], tail ? 1 : 0,
                                      blocks * 256.0 / 48000.0);
        if (!okNow) {
          if (bad++ < 25)
            SND_CHECK_MSG(false, "%s %s=%s (%s): silence->%g, loud peak %g, tail %s after %.1f s, after-tail peak %g", Short(kind),
                          pname, p >= 0 ? vname[variant] : "-", viaBlock ? "ParamBlock" : "bank", static_cast<double>(out),
                          static_cast<double>(loud), tail ? "done" : "NOT done", blocks * 256.0 / 48000.0,
                          static_cast<double>(after));
        }
      }
    }
  }
  SND_CHECK_EQ(bad, 0u);
}

// Settings that must leave the signal untouched (bit-exact, or within the
// rounding of the documented formula).
SND_TEST(EffectsIndep, NeutralSettingsPassTheSignal) {
  struct Case {
    EffectKind kind;
    std::vector<std::pair<std::uint32_t, float>> ov;
    float tol;
    const char* what;
  };
  const Case cases[] = {
      {EffectKind::Echo, {{2, 1.0f}, {3, 0.0f}}, 0.0f, "WetMix 0"},
      {EffectKind::Delay, {{2, 0.0f}, {3, -96.0f}}, 3e-5f, "WetDb -96 (= -96 dB, not off)"},
      {EffectKind::Chorus, {{0, 1.0f}, {1, 0.0f}}, 0.0f, "WetMix 0"},
      {EffectKind::Flanger, {{0, 1.0f}, {1, 0.0f}}, 0.0f, "WetMix 0"},
      {EffectKind::Distortion, {{1, 0.0f}}, 0.0f, "Mix 0"},
      {EffectKind::Compressor, {{1, 1.0f}}, 0.0f, "Ratio 1"},
      {EffectKind::ParamEq, {}, 0.0f, "defaults (all bands peaking 0 dB)"},
      {EffectKind::ParamEq, {{0, 0.0f}, {4, 0.0f}, {8, 0.0f}, {12, 0.0f}}, 0.0f, "all bands off"},
      {EffectKind::PitchShifter, {{2, 0.0f}}, 0.0f, "Mix 0"},
      {EffectKind::Surround, {{0, 1.0f}}, 1e-6f, "Width 1 (stereo)"},
      {EffectKind::Reverb, {{1, -10000.0f}, {6, -10000.0f}, {8, -10000.0f}}, 1e-6f, "Room/Reflections/Reverb -10000 mB"},
  };
  for (const Case& cs : cases) {
    Fx fx(cs.kind, 2, 256, 48000, cs.ov);
    SND_REQUIRE(fx.ok);
    Audio a(2, 256), ref(2, 256);
    float worst = 0.0f;
    for (int b = 0; b < 100; ++b) {
      Material(a, static_cast<std::uint64_t>(b) * 256u, 256, 0.9f);
      Material(ref, static_cast<std::uint64_t>(b) * 256u, 256, 0.9f);
      Run(fx, a.cp(), a.p(), 2, 256);
      for (std::uint32_t c = 0; c < 2; ++c) worst = std::max(worst, dsptest::MaxAbsDiff(a.ch(c), ref.ch(c), 256));
    }
    SND_CHECK_MSG(worst <= cs.tol, "%s %s: differs from the input by %g", Short(cs.kind), cs.what, static_cast<double>(worst));
  }
}

// in == out (in place) gives the same bits as separate buffers, with ramps and sidechain.
SND_TEST(EffectsIndep, InPlaceEqualsOutOfPlace) {
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    const EffectInfo& info = GetEffectInfo(kind);
    for (std::uint32_t ch : {1u, 2u, 6u, 8u}) {
      Fx a(kind, ch, 128, 48000), b(kind, ch, 128, 48000);
      SND_REQUIRE(a.ok && b.ok);
      Audio x(ch, 300), y(ch, 300), z(ch, 300), sc(1, 300);
      std::uint32_t diff = 0;
      float s[kMaxEffectParams], e[kMaxEffectParams];
      EffectDefaultParams(kind, s);
      EffectDefaultParams(kind, e);
      for (int blk = 0; blk < 12; ++blk) {
        for (std::uint32_t p = 0; p < info.paramCount; ++p) {
          if (info.params[p].flags & (kParamInitOnly | kParamInteger)) continue;
          s[p] = e[p];
          const float f = static_cast<float>((blk * 7 + p * 3) % 10) / 9.0f;
          e[p] = info.params[p].minValue + (info.params[p].maxValue - info.params[p].minValue) * f;
        }
        snd::ParamBlock pb{s, e, info.paramCount, 0xFFFFFFFFu, nullptr};
        Material(x, static_cast<std::uint64_t>(blk) * 300u, 300);
        Material(y, static_cast<std::uint64_t>(blk) * 300u, 300);
        Material(sc, static_cast<std::uint64_t>(blk) * 300u + 9, 300, 0.9f);
        EffectSidechain side{sc.cp(), 1};
        Run(a, x.cp(), x.p(), ch, 300, &pb, &side);   // in place (300 > max_frames: sub-blocks)
        Run(b, y.cp(), z.p(), ch, 300, &pb, &side);   // separate
        for (std::uint32_t c = 0; c < ch; ++c) diff += dsptest::MaxAbsDiff(x.ch(c), z.ch(c), 300) != 0.0f;
      }
      SND_CHECK_MSG(diff == 0, "%s %u ch: in-place differs from out-of-place in %u channel-blocks", Short(kind), ch, diff);
    }
  }
}

// Constant parameters: 1 frame per call and random splits give the same bits
// as one call (6 channels, sidechain for the compressor).
SND_TEST(EffectsIndep, BlockSplitSingleFramesAndRandom) {
  constexpr std::uint32_t kN = 6000;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    Fx a(kind, 6, 512, 48000), b(kind, 6, 512, 48000), c(kind, 6, 512, 48000);
    SND_REQUIRE(a.ok && b.ok && c.ok);
    Audio xa(6, kN), xb(6, kN), xc(6, kN), sc(1, kN);
    Material(xa, 0, kN);
    Material(xb, 0, kN);
    Material(xc, 0, kN);
    Material(sc, 99, kN, 1.0f);
    for (std::uint32_t i = 3000; i < kN; ++i)  // silence in the second half: tail handling too
      for (std::uint32_t ch = 0; ch < 6; ++ch) xa.ch(ch)[i] = xb.ch(ch)[i] = xc.ch(ch)[i] = 0.0f;
    for (std::uint32_t done = 0; done < kN; done += 500) {
      const float* in[6];
      float* out[6];
      for (std::uint32_t ch = 0; ch < 6; ++ch) out[ch] = xa.ch(ch) + done, in[ch] = out[ch];
      const float* scp[1] = {sc.ch(0) + done};
      EffectSidechain side{scp, 1};
      Run(a, in, out, 6, 500, nullptr, &side);
    }
    for (std::uint32_t i = 0; i < kN; ++i) {
      const float* in[6];
      float* out[6];
      for (std::uint32_t ch = 0; ch < 6; ++ch) out[ch] = xb.ch(ch) + i, in[ch] = out[ch];
      const float* scp[1] = {sc.ch(0) + i};
      EffectSidechain side{scp, 1};
      Run(b, in, out, 6, 1, nullptr, &side);
    }
    std::uint32_t seed = 3;
    for (std::uint32_t done = 0; done < kN;) {
      seed = seed * 1103515245u + 12345u;
      const std::uint32_t n = std::min<std::uint32_t>(1 + (seed >> 16) % 700, kN - done);
      const float* in[6];
      float* out[6];
      for (std::uint32_t ch = 0; ch < 6; ++ch) out[ch] = xc.ch(ch) + done, in[ch] = out[ch];
      const float* scp[1] = {sc.ch(0) + done};
      EffectSidechain side{scp, 1};
      Run(c, in, out, 6, n, nullptr, &side);
      done += n;
    }
    float d1 = 0.0f, d2 = 0.0f;
    for (std::uint32_t ch = 0; ch < 6; ++ch) {
      d1 = std::max(d1, dsptest::MaxAbsDiff(xa.ch(ch), xb.ch(ch), kN));
      d2 = std::max(d2, dsptest::MaxAbsDiff(xa.ch(ch), xc.ch(ch), kN));
    }
    // effects.hpp promises no bit-exactness across splits; the only documented
    // block-granular behaviour is the tail clear below -100 dBFS (1e-5).
    if (d1 != 0.0f || d2 != 0.0f)
      snd::test::Note("%-11s split: 1-frame calls differ by %g, random splits by %g (tail clear at block granularity)",
                      Short(kind), static_cast<double>(d1), static_cast<double>(d2));
    SND_CHECK_MSG(d1 < 1e-5f && d2 < 1e-5f, "%s: 1-frame calls differ by %g, random splits by %g", Short(kind),
                  static_cast<double>(d1), static_cast<double>(d2));
  }
}

// D-26 at the effect level: a block whose start value equals the previous
// block's end value continues without a step. Every rampable parameter is
// swept min -> max over 16 blocks of 128 frames (start = previous end) under a
// 200 Hz sine; the second difference at block boundaries must not stand out
// from the one inside the blocks. Integer / init-only parameters are skipped.
SND_TEST(EffectsIndep, ParameterRampsHaveNoStepAtBlockBoundaries) {
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    const EffectInfo& info = GetEffectInfo(kind);
    for (std::uint32_t p = 0; p < info.paramCount; ++p) {
      const EffectParamInfo& pi = info.params[p];
      if (pi.flags & (kParamInitOnly | kParamInteger)) continue;
      Fx fx(kind, 1, 128, 48000);
      SND_REQUIRE(fx.ok);
      float s[kMaxEffectParams], e[kMaxEffectParams];
      EffectDefaultParams(kind, s);
      EffectDefaultParams(kind, e);
      Audio a(1, 128);
      std::vector<float> out;
      // Settle 0.2 s at the start value, then sweep.
      for (int b = 0; b < 16 + 75; ++b) {
        const int r = std::max(0, b - 75);
        s[p] = pi.minValue + (pi.maxValue - pi.minValue) * static_cast<float>(r) / 16.0f;
        e[p] = pi.minValue + (pi.maxValue - pi.minValue) * static_cast<float>(b < 75 ? 0 : r + 1) / 16.0f;
        snd::ParamBlock pb{s, e, info.paramCount, 1u << p, nullptr};
        for (std::uint32_t i = 0; i < 128; ++i)
          a.ch(0)[i] = 0.5f * static_cast<float>(std::sin(2 * dsptest::kPi * 200.0 * (b * 128 + i) / 48000.0));
        Run(fx, a.cp(), a.p(), 1, 128, &pb);
        if (b >= 74) out.insert(out.end(), a.ch(0), a.ch(0) + 128);
      }
      // A step S between frames j-1 and j shows as second differences +S at j
      // and -S at j + 1 (a slope change only gives one spike), so the step
      // estimate is the smaller of two adjacent opposite-sign second differences.
      std::vector<float> d2(out.size(), 0.0f);
      for (std::size_t i = 2; i < out.size(); ++i) d2[i] = out[i] - 2.0f * out[i - 1] + out[i - 2];
      float atB = 0.0f, inside = 0.0f;
      for (std::size_t i = 2; i + 1 < out.size(); ++i) {
        const float step = (d2[i] > 0.0f) != (d2[i + 1] > 0.0f) ? std::min(std::fabs(d2[i]), std::fabs(d2[i + 1])) : 0.0f;
        if (i % 128 == 0) {
          atB = std::max(atB, step);
        } else {
          inside = std::max(inside, std::fabs(d2[i]));
        }
      }
      if (atB > 2.0f * inside + 1e-3f) {
        if (bad++ < 20)
          SND_CHECK_MSG(false, "%s %s sweep: step of ~%.4g at a block boundary (largest second difference inside blocks %.4g)",
                        Short(kind), pi.name, static_cast<double>(atB), static_cast<double>(inside));
      }
    }
  }
  SND_CHECK_EQ(bad, 0u);
}

// After the tail ended and the history was cleared, new input sounds exactly
// like a fresh instance (time-invariant effects); and EffectReset after loud
// input gives the fresh-instance output for every effect.
SND_TEST(EffectsIndep, ResumeAfterTailAndResetMatchFreshInstance) {
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    for (int mode = 0; mode < 2; ++mode) {  // 0: tail, 1: reset
      Fx used(kind, 2, 256, 48000), fresh(kind, 2, 256, 48000);
      SND_REQUIRE(used.ok && fresh.ok);
      Audio a(2, 256), f(2, 256);
      for (int b = 0; b < 40; ++b) {
        Material(a, static_cast<std::uint64_t>(b) * 256u, 256, 1.0f);
        Run(used, a.cp(), a.p(), 2, 256);
      }
      if (mode == 0) {
        std::uint32_t blocks = 0;
        while (!EffectIsTailDone(used.state()) && blocks++ < 48000u * 40u / 256u) {
          a.Zero();
          Run(used, a.cp(), a.p(), 2, 256);
        }
      } else {
        EffectReset(used.state());
        SND_CHECK(EffectIsTailDone(used.state()));
      }
      float diff = 0.0f;
      for (int b = 0; b < 40; ++b) {
        Material(a, 1000000u + static_cast<std::uint64_t>(b) * 256u, 256, 0.7f);
        Material(f, 1000000u + static_cast<std::uint64_t>(b) * 256u, 256, 0.7f);
        Run(used, a.cp(), a.p(), 2, 256);
        Run(fresh, f.cp(), f.p(), 2, 256);
        for (std::uint32_t c = 0; c < 2; ++c) diff = std::max(diff, dsptest::MaxAbsDiff(a.ch(c), f.ch(c), 256));
      }
      // Effects whose state legitimately carries over silence: LFO phase
      // (chorus, flanger, pitch shifter), envelopes still releasing
      // (compressor, limiter, normalize) after the tail check.
      const bool phaseOrEnvelope = kind == EffectKind::Chorus || kind == EffectKind::Flanger ||
                                   kind == EffectKind::PitchShifter || kind == EffectKind::Compressor ||
                                   kind == EffectKind::Limiter || kind == EffectKind::Normalize;
      if (mode == 0 && phaseOrEnvelope) {
        snd::test::Note("%-11s resume after tail vs fresh: max diff %g (state carried over silence)", Short(kind),
                        static_cast<double>(diff));
        continue;
      }
      if (mode == 1 && (kind == EffectKind::Chorus || kind == EffectKind::Flanger)) {
        snd::test::Note("%-11s after EffectReset vs fresh: max diff %g (LFO phase is not reset; spec does not say)", Short(kind),
                        static_cast<double>(diff));
        continue;
      }
      SND_CHECK_MSG(diff == 0.0f, "%s: after %s the output differs from a fresh instance by %g", Short(kind),
                    mode == 0 ? "the tail ended" : "EffectReset", static_cast<double>(diff));
    }
  }
}

// EffectProcessBatch == EffectProcess per item (mixed channel counts, params, sidechains).
SND_TEST(EffectsIndep, BatchEqualsSingle) {
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    const EffectInfo& info = GetEffectInfo(kind);
    constexpr std::uint32_t kItems = 5;
    std::vector<std::unique_ptr<Fx>> ba, sa;
    std::vector<std::unique_ptr<Audio>> xa, xs;
    std::vector<std::vector<float>> s(kItems, std::vector<float>(kMaxEffectParams)), e(kItems, std::vector<float>(kMaxEffectParams));
    std::vector<snd::ParamBlock> pbs(kItems);
    Audio sc(2, 200);
    Material(sc, 4, 200, 1.0f);
    EffectSidechain side{sc.cp(), 2};
    std::vector<EffectBatchItem> items(kItems);
    for (std::uint32_t i = 0; i < kItems; ++i) {
      const std::uint32_t ch = 1 + (i * 3) % 8;
      ba.push_back(std::make_unique<Fx>(kind, ch, 64, 48000));
      sa.push_back(std::make_unique<Fx>(kind, ch, 64, 48000));
      xa.push_back(std::make_unique<Audio>(ch, 200));
      xs.push_back(std::make_unique<Audio>(ch, 200));
      Material(*xa.back(), i * 1000, 200);
      Material(*xs.back(), i * 1000, 200);
      EffectDefaultParams(kind, s[i].data());
      EffectDefaultParams(kind, e[i].data());
      for (std::uint32_t p = 0; p < info.paramCount; ++p)
        e[i][p] = info.params[p].minValue + (info.params[p].maxValue - info.params[p].minValue) * 0.1f * static_cast<float>(i + p % 3);
      pbs[i] = snd::ParamBlock{s[i].data(), e[i].data(), info.paramCount, 0xFFFFFFFFu, nullptr};
      items[i] = EffectBatchItem{ba[i]->state(), xa[i]->cp(), xa[i]->p(), ch, 200, i % 2 ? &pbs[i] : nullptr, i == 2 ? &side : nullptr};
    }
    {
      SND_RT_SCOPE();
      EffectProcessBatch(kind, items.data(), kItems);
    }
    for (std::uint32_t i = 0; i < kItems; ++i) {
      Run(*sa[i], xs[i]->cp(), xs[i]->p(), xs[i]->channels(), 200, i % 2 ? &pbs[i] : nullptr, i == 2 ? &side : nullptr);
      float d = 0.0f;
      for (std::uint32_t c = 0; c < xs[i]->channels(); ++c) d = std::max(d, dsptest::MaxAbsDiff(xa[i]->ch(c), xs[i]->ch(c), 200));
      SND_CHECK_MSG(d == 0.0f, "%s batch item %u differs by %g", Short(kind), i, static_cast<double>(d));
    }
  }
}

namespace {
// C plugin mock: out = in * gain + offset, where gain = params->end[0] (1 if
// absent). Records the calls; state = {gain, calls}.
struct PluginRec {
  std::uint32_t calls = 0;
  std::uint32_t lastChannels = 0;
  bool aliased = true;
  float offset = 0.0f;
};
std::uint32_t SND_CALL MockStateBytes(void*, const snd_effect_desc* d) { return 8u + 4u * d->channels; }
void SND_CALL MockInit(void*, void* state, const snd_effect_desc*, std::uint32_t) { std::memset(state, 0, 8); }
void SND_CALL MockProcess(void* user, void* state, const float* const* in, float* const* out, std::uint32_t channels,
                          std::uint32_t frames, const snd_param_block* params) {
  auto* rec = static_cast<PluginRec*>(user);
  rec->calls++;
  rec->lastChannels = channels;
  const float g = (params != nullptr && params->count > 0 && params->end != nullptr) ? params->end[0] : 1.0f;
  for (std::uint32_t c = 0; c < channels; ++c) {
    rec->aliased = rec->aliased && in[c] == out[c];
    for (std::uint32_t i = 0; i < frames; ++i) out[c][i] = in[c][i] * g + rec->offset;
  }
  static_cast<float*>(state)[0] = g;
}
std::int32_t SND_CALL MockTail(void*, const void* state) { return static_cast<const float*>(state)[0] == 0.0f ? 1 : 0; }
}  // namespace

// Chains: order, bypass, plugins mixed with standard effects, null process.
SND_TEST(EffectsIndep, ChainOrderBypassAndPlugins) {
  PluginRec rec;
  rec.offset = 0.25f;
  const snd::Id pid = snd::HashId("indep.mock");
  snd_dsp_effect plug{&rec, pid, 0, &MockStateBytes, &MockInit, &MockProcess, &MockTail};
  snd::EffectDesc pd{pid, 2, 256, 0, nullptr, nullptr, 0};
  const std::uint32_t pbytes = PluginStateBytes(plug, pd);
  SND_CHECK_EQ(pbytes, 64u);
  dsptest::AlignedMem pmem(pbytes);
  plug.init(plug.user, pmem.get(), &pd, 48000);
  // Distortion (nonlinear) so the order matters: chain = [plugin, distortion] vs [distortion, plugin].
  Fx d1(EffectKind::Distortion, 2, 256, 48000), d2(EffectKind::Distortion, 2, 256, 48000), d3(EffectKind::Distortion, 2, 256, 48000);
  float g = 2.0f;
  snd::ParamBlock gp{&g, &g, 1, 1u, nullptr};
  Audio x(2, 256), y(2, 256), ref(2, 256);
  Material(x, 0, 256);
  Material(y, 0, 256);
  Material(ref, 0, 256);
  EffectSlot ab[2] = {{pid, &plug, pmem.get(), &gp, nullptr, 0, 0}, {kEffectDistortion, nullptr, d1.state(), nullptr, nullptr, 0, 0}};
  EffectSlot ba[2] = {{kEffectDistortion, nullptr, d2.state(), nullptr, nullptr, 0, 0}, {pid, &plug, pmem.get(), &gp, nullptr, 0, 0}};
  {
    SND_RT_SCOPE();
    ProcessEffectChain(ab, 2, x.p(), 2, 256);
    ProcessEffectChain(ba, 2, y.p(), 2, 256);
  }
  // Manual reference for ab: plugin then distortion.
  for (std::uint32_t c = 0; c < 2; ++c)
    for (std::uint32_t i = 0; i < 256; ++i) ref.ch(c)[i] = ref.ch(c)[i] * 2.0f + 0.25f;
  Run(d3, ref.cp(), ref.p(), 2, 256);
  for (std::uint32_t c = 0; c < 2; ++c) SND_CHECK_EQ(dsptest::MaxAbsDiff(x.ch(c), ref.ch(c), 256), 0.0f);
  SND_CHECK(dsptest::MaxAbsDiff(x.ch(0), y.ch(0), 256) > 0.01f);
  SND_CHECK_EQ(rec.calls, 2u);
  SND_CHECK(rec.aliased);  // the chain runs in place
  // Bypassed slots and a plugin without process are skipped; null params -> empty block.
  snd_dsp_effect noProc = plug;
  noProc.process = nullptr;
  EffectSlot mix[3] = {{pid, &plug, pmem.get(), nullptr, nullptr, 0, 0},
                       {kEffectDistortion, nullptr, d1.state(), nullptr, nullptr, 1, 0},
                       {pid, &noProc, pmem.get(), &gp, nullptr, 0, 0}};
  Material(x, 0, 256);
  Material(ref, 0, 256);
  ProcessEffectChain(mix, 3, x.p(), 2, 256);
  for (std::uint32_t c = 0; c < 2; ++c)
    for (std::uint32_t i = 0; i < 256; ++i) SND_CHECK_EQ(x.ch(c)[i], ref.ch(c)[i] + 0.25f);  // gain 1 (no params) + offset
  SND_CHECK_EQ(rec.calls, 3u);
  SND_CHECK(EffectSlotIsTailDone(mix[1]) == EffectIsTailDone(d1.state()));
  SND_CHECK(!EffectSlotIsTailDone(mix[0]));  // plugin state gain 1 -> not done
  EffectSlot empty{pid, &plug, nullptr, nullptr, nullptr, 0, 0};
  SND_CHECK(EffectSlotIsTailDone(empty));
  // A chain wider than the standard effect's channel count leaves the extra channels alone.
  Fx mono(EffectKind::Distortion, 1, 256, 48000);
  Audio w(3, 256), wr(3, 256);
  Material(w, 0, 256);
  Material(wr, 0, 256);
  EffectSlot one{kEffectDistortion, nullptr, mono.state(), nullptr, nullptr, 0, 0};
  ProcessEffectChain(&one, 1, w.p(), 3, 256);
  SND_CHECK(dsptest::MaxAbsDiff(w.ch(0), wr.ch(0), 256) > 0.0f);
  SND_CHECK_EQ(dsptest::MaxAbsDiff(w.ch(1), wr.ch(1), 256), 0.0f);
  SND_CHECK_EQ(dsptest::MaxAbsDiff(w.ch(2), wr.ch(2), 256), 0.0f);
}

// DSPN: instanceBytes = sum of 64-rounded instance sizes; laying the instances
// out back to back in a pool of exactly that size works (no instance reaches
// into its neighbour or past the pool).
SND_TEST(EffectsIndep, PoolNeedMatchesPackedInstances) {
  PluginRec rec;
  const snd::Id pid = snd::HashId("indep.mock");
  snd_dsp_effect plugs[2] = {{&rec, pid, 0, &MockStateBytes, &MockInit, &MockProcess, &MockTail},
                             {&rec, kEffectEcho, 0, &MockStateBytes, &MockInit, &MockProcess, &MockTail}};  // overrides echo
  std::vector<snd::EffectDesc> descs;
  std::vector<std::vector<float>> params;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    for (std::uint32_t ch : {1u, 5u, 8u}) {
      const EffectInfo& info = GetEffectInfo(static_cast<EffectKind>(k));
      params.emplace_back(kMaxEffectParams);
      EffectDefaultParams(static_cast<EffectKind>(k), params.back().data());
      for (std::uint32_t p = 0; p < info.paramCount; ++p)
        if (info.params[p].flags & kParamInitOnly) params.back()[p] = info.params[p].maxValue;
      descs.push_back(snd::EffectDesc{info.typeId, ch, 200, info.paramCount, nullptr, nullptr, 0});
    }
  }
  for (std::size_t i = 0; i < descs.size(); ++i) descs[i].initial_params = params[i].data();
  descs.push_back(snd::EffectDesc{pid, 3, 200, 0, nullptr, nullptr, 0});
  descs.push_back(snd::EffectDesc{snd::HashId("nobody"), 3, 200, 0, nullptr, nullptr, 0});
  descs.push_back(snd::EffectDesc{kEffectReverb, 0, 200, 0, nullptr, nullptr, 0});  // invalid
  std::uint32_t unknown = 0;
  const auto need = ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()), 96000, plugs, 2, &unknown);
  SND_CHECK_EQ(unknown, 2u);
  SND_CHECK_EQ(need.instanceCount, static_cast<std::uint32_t>(descs.size()) - 2u);
  std::uint64_t sum = 0;
  for (const auto& d : descs) {
    const std::uint32_t b = EffectInstanceBytes(d, 96000, plugs, 2);
    SND_CHECK_EQ(b % 64u, 0u);
    sum += b;
  }
  SND_CHECK_EQ(need.instanceBytes, sum);
  // Echo ids resolve to the plugin (registration overrides the standard kind).
  SND_CHECK_EQ(EffectInstanceBytes(descs[3], 96000, plugs, 2), 64u);
  // Pack and run everything.
  dsptest::AlignedMem pool(need.instanceBytes + 256);
  auto* base = static_cast<std::uint8_t*>(pool.get());
  std::memset(base, 0, need.instanceBytes);  // zeroed pool (dirty memory: see InitOnDirtyMemory)
  std::memset(base + need.instanceBytes, kGuardByte, 256);
  std::vector<std::uint64_t> offs;
  std::uint64_t off = 0;
  for (const auto& d : descs) {
    const std::uint32_t b = EffectInstanceBytes(d, 96000, plugs, 2);
    offs.push_back(off);
    if (b == 0) continue;
    const EffectInfo* info = FindEffect(d.effect_type);
    const bool isPlugin = d.effect_type == pid || d.effect_type == kEffectEcho;
    if (isPlugin) {
      plugs[0].init(&rec, base + off, &d, 96000);
    } else {
      SND_REQUIRE(info != nullptr);
      SND_CHECK(EffectInit(base + off, b, d, 96000));
    }
    off += b;
  }
  Audio a(8, 600);
  for (std::size_t i = 0; i < descs.size(); ++i) {
    const auto& d = descs[i];
    if (EffectInstanceBytes(d, 96000, plugs, 2) == 0 || d.effect_type == pid || d.effect_type == kEffectEcho) continue;
    Material(a, i * 600, 600, 1.0f, 96000);
    SND_RT_SCOPE();
    EffectProcess(base + offs[i], a.cp(), a.p(), d.channels, 600, nullptr);
  }
  std::uint32_t guardBad = 0;
  for (std::uint32_t i = 0; i < 256; ++i) guardBad += base[need.instanceBytes + i] != kGuardByte;
  SND_CHECK_EQ(guardBad, 0u);
  // Every standard instance still knows its kind (no neighbour overwrote its header).
  for (std::size_t i = 0; i < descs.size(); ++i) {
    const auto& d = descs[i];
    if (EffectInstanceBytes(d, 96000, plugs, 2) == 0 || d.effect_type == pid || d.effect_type == kEffectEcho) continue;
    SND_CHECK(GetEffectInfo(EffectStateKind(base + offs[i])).typeId == d.effect_type);
  }
}

// A call with 0 channels (an empty bus) must not leave the state unusable.
SND_TEST(EffectsIndep, ZeroChannelCallDoesNotPoisonState) {
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    Fx fx(kind, 2, 256, 48000);
    SND_REQUIRE(fx.ok);
    Audio a(2, 256);
    Material(a, 0, 256);
    Run(fx, a.cp(), a.p(), 0, 256);
    float worst = 0.0f;
    for (int b = 0; b < 20; ++b) {
      Material(a, static_cast<std::uint64_t>(b) * 256u, 256);
      Run(fx, a.cp(), a.p(), 2, 256);
      worst = std::max(worst, MaxAbs(a, 256));
    }
    SND_CHECK_MSG(std::isfinite(worst), "%s: a 0-channel call made later output non-finite", Short(kind));
  }
}

// RT safety of the remaining entry points (EffectReset, EffectIsTailDone,
// EffectParamValue, EffectLatencyFrames, ProcessEffectChain with plugins,
// EffectSlotIsTailDone) under SND_RT_SCOPE.
SND_TEST(EffectsIndep, RemainingEntryPointsAreRtSafe) {
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    Fx fx(static_cast<EffectKind>(k), 2, 64, 48000);
    SND_REQUIRE(fx.ok);
    Audio a(2, 64);
    Material(a, 0, 64);
    EffectSlot slot{GetEffectInfo(static_cast<EffectKind>(k)).typeId, nullptr, fx.state(), nullptr, nullptr, 0, 0};
    SND_RT_SCOPE();
    ProcessEffectChain(&slot, 1, a.p(), 2, 64);
    (void)EffectSlotIsTailDone(slot);
    (void)EffectIsTailDone(fx.state());
    (void)EffectParamValue(fx.state(), 0);
    (void)EffectLatencyFrames(fx.state());
    (void)EffectStateKind(fx.state());
    EffectReset(fx.state());
  }
}

// EffectInit must not depend on the previous contents of the DSP pool memory
// (3.3: the pool is reused across bank loads; IAllocator memory is not zeroed).
// Each kind is initialised on memory filled with 0xA5 / 0xFF and compared with
// an instance initialised on zeroed memory. Runs in a child process because an
// uninitialised ring index makes the effect read and write far outside its state.
SND_TEST(EffectsIndep, InitOnDirtyMemoryMatchesZeroedMemory) {
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    const EffectKind kind = static_cast<EffectKind>(k);
    for (std::uint8_t fill : {std::uint8_t{0xA5}, std::uint8_t{0xFF}}) {
      // Prepare both instances in the parent (no allocation in the child).
      Fx clean(kind, 2, 256, 48000);
      Fx dirty(kind, 2, 256, 48000);
      SND_REQUIRE(clean.ok);
      std::memset(dirty.state(), fill, dirty.bytes);
      const bool initOk = EffectInit(dirty.state(), dirty.bytes, dirty.desc, 48000);
      SND_REQUIRE(initOk);
      Audio a(2, 256), b(2, 256);
      std::fflush(nullptr);
      const pid_t pid = fork();
      if (pid == 0) {
        int rc = 0;
        for (int blk = 0; blk < 400 && rc == 0; ++blk) {  // 2.1 s: longer than the default delays
          Material(a, static_cast<std::uint64_t>(blk) * 256u, 256);
          Material(b, static_cast<std::uint64_t>(blk) * 256u, 256);
          EffectProcess(clean.state(), a.cp(), a.p(), 2, 256, nullptr);
          EffectProcess(dirty.state(), b.cp(), b.p(), 2, 256, nullptr);
          for (std::uint32_t c = 0; c < 2; ++c)
            if (std::memcmp(a.ch(c), b.ch(c), 256 * sizeof(float)) != 0) rc = 3;
        }
        if (!dirty.GuardsOk()) rc = 4;
        _exit(rc);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      const char* what = nullptr;
      if (WIFSIGNALED(status)) {
        what = "crashed (signal)";
      } else if (WEXITSTATUS(status) == 3) {
        what = "output differs from an instance on zeroed memory";
      } else if (WEXITSTATUS(status) == 4) {
        what = "wrote outside its state";
      } else if (WEXITSTATUS(status) != 0) {
        what = "failed";
      }
      if (what != nullptr) {
        ++bad;
        SND_CHECK_MSG(false, "%s initialised on memory filled with 0x%02X: %s (signal %d)", Short(kind), fill, what,
                      WIFSIGNALED(status) ? WTERMSIG(status) : 0);
      }
    }
  }
  SND_CHECK_EQ(bad, 0u);
}

// Cross-build numeric comparison (gcc / clang, Debug / Release, scalar / SIMD):
// with SND_INDEP_DUMP=<file> set, writes the outputs of every effect, the sinc
// resampler through a ratio glide, the voice filter and the loudness meter as
// raw float32 for an offline diff between builds. A no-op otherwise.
SND_TEST(EffectsIndep, DumpForCrossBuildComparison) {
  const char* path = std::getenv("SND_INDEP_DUMP");
  if (path == nullptr) return;
  std::vector<float> dump;
  for (std::uint32_t k = 0; k < kKindCount; ++k) {
    Fx fx(static_cast<EffectKind>(k), 2, 256, 48000);
    Audio a(2, 256);
    for (int b = 0; b < 40; ++b) {
      Material(a, static_cast<std::uint64_t>(b) * 256u, 256, 0.8f);
      Run(fx, a.cp(), a.p(), 2, 256);
      for (std::uint32_t c = 0; c < 2; ++c) dump.insert(dump.end(), a.ch(c), a.ch(c) + 256);
    }
  }
  for (ResamplerQuality q : {ResamplerQuality::Linear, ResamplerQuality::Cubic, ResamplerQuality::Sinc}) {
    dsptest::AlignedMem st(ResamplerStateBytes(q, 1));
    ResamplerInit(st.get(), q, 1);
    std::uint64_t pos = 0;
    for (int b = 0; b < 40; ++b) {
      const float r0 = 0.6f + 0.05f * b, r1 = 0.6f + 0.05f * (b + 1);
      const std::uint32_t n = ResamplerInputFrames(st.get(), 256, r0, r1);
      std::vector<float> in(ResamplerPadFrames(q) + n), out(256);
      for (std::uint32_t i = 0; i < n; ++i)
        in[ResamplerPadFrames(q) + i] = static_cast<float>(std::sin(0.07 * static_cast<double>(pos + i)) * 0.7);
      float* ip[1] = {in.data()};
      float* op[1] = {out.data()};
      pos += ResamplerProcess(st.get(), ip, n, op, 256, r0, r1);
      dump.insert(dump.end(), out.begin(), out.end());
    }
  }
  snd::FilterState fs;
  VoiceFilterInit(fs, snd::FilterTarget{800.0f, 100.0f});
  Audio a(1, 256);
  for (int b = 0; b < 20; ++b) {
    Material(a, static_cast<std::uint64_t>(b) * 256u, 256);
    VoiceFilterProcess(fs, a.p(), 1, 256, snd::FilterTarget{800.0f + 300.0f * b, 100.0f}, 48000);
    dump.insert(dump.end(), a.ch(0), a.ch(0) + 256);
  }
  auto lm = std::make_unique<LoudnessMeterState>();
  LoudnessMeterInit(*lm, 1, 48000);
  for (int b = 0; b < 400; ++b) {
    Material(a, static_cast<std::uint64_t>(b) * 256u, 256);
    LoudnessMeterProcess(*lm, a.cp(), 256);
  }
  dump.push_back(LoudnessIntegrated(*lm));
  dump.push_back(LoudnessShortTerm(*lm));
  FILE* f = std::fopen(path, "wb");
  SND_REQUIRE(f != nullptr);
  std::fwrite(dump.data(), sizeof(float), dump.size(), f);
  std::fclose(f);
  snd::test::Note("wrote %zu floats to %s", dump.size(), path);
}
