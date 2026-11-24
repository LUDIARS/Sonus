// Independent tests of the WSOLA time stretch (time_stretch.hpp, CONTRACTS.md
// D-209 / D-210), written from the specification: the shape formulas, guards
// around the state / input / output, the input requirement (pure, exact, too
// little / too much input), long-run consumption without drift, speed 1 bit
// exactness and the constant offset after other speeds, clamping of
// out-of-range speeds, block splitting, ramps against an own simulation of the
// hop rule, pitch and amplitude of sinusoids (50 Hz .. 10 kHz), transients,
// multichannel coherence, silence / DC / full scale / denormals / NaN, Reset,
// rates 8 kHz .. 384 kHz with 8 channels, RT safety and the cost per block.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/time_stretch.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr double kPi = dsptest::kPi;
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kGuardBytes = 256;
constexpr unsigned char kGuardByte = 0xA5;
using Signal = std::vector<std::vector<float>>;  // [channel][frame]

// State with a guard after StateBytes.
struct Ts {
  std::vector<unsigned char> raw;
  unsigned char* p = nullptr;
  std::size_t bytes = 0;
  std::uint32_t ch;
  std::uint64_t pos = 0;  // next unread source frame
  bool ok = false;
  Ts(std::uint32_t channels, std::uint32_t rate = kRate, float maxSpeed = kTimeStretchMaxSpeed,
     float win = kTimeStretchDefaultWindowMs, float search = kTimeStretchDefaultSearchMs)
      : ch(channels) {
    bytes = TimeStretchStateBytes(channels, rate, maxSpeed, win, search);
    raw.assign(bytes + kGuardBytes + 64u, 0xFF);  // dirty
    p = raw.data() + (64u - reinterpret_cast<std::uintptr_t>(raw.data()) % 64u);
    std::memset(p + bytes, kGuardByte, kGuardBytes);
    ok = bytes > 0u && TimeStretchInit(p, bytes, channels, rate, maxSpeed, win, search);
  }
  void* st() const { return p; }
  bool GuardIntact() const {
    for (std::size_t i = 0; i < kGuardBytes; ++i)
      if (p[bytes + i] != kGuardByte) return false;
    return true;
  }
  // One call pulling from `src` (zeros past the end); appends to `out`. Checks
  // the guards of the input and output buffers; `bad` counts violations.
  std::uint32_t Call(const Signal& src, Signal* out, std::uint32_t frames, float s0, float s1,
                     int* bad = nullptr) {
    const std::uint32_t n = TimeStretchInputFrames(st(), frames, s0, s1);
    const std::uint32_t pad = TimeStretchPadFrames(st());
    constexpr std::uint32_t g = 16;
    std::vector<std::vector<float>> inb(ch, std::vector<float>(pad + n + g, 7.25f));
    std::vector<std::vector<float>> ob(ch, std::vector<float>(frames + g, 3.5f));
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
    std::vector<std::vector<float>> copy(inb);
    std::uint32_t used;
    {
      SND_RT_SCOPE();
      used = TimeStretchProcess(st(), ip, n, op, frames, s0, s1);
    }
    if (bad != nullptr) {
      if (used != n) ++*bad;
      for (std::uint32_t c = 0; c < ch; ++c) {
        for (std::uint32_t i = pad; i < pad + n + g; ++i)
          if (std::memcmp(&inb[c][i], &copy[c][i], 4) != 0) {
            ++*bad;  // the input frames (not the pad) or the guard were written
            break;
          }
        for (std::uint32_t i = frames; i < frames + g; ++i)
          if (ob[c][i] != 3.5f) {
            ++*bad;
            break;
          }
      }
    }
    pos += used;
    for (std::uint32_t c = 0; c < ch; ++c) (*out)[c].insert((*out)[c].end(), ob[c].begin(), ob[c].begin() + frames);
    return used;
  }
  // Runs `total` output frames in blocks of `block` at constant speed.
  Signal Run(const Signal& src, std::uint32_t total, std::uint32_t block, float speed, int* bad = nullptr) {
    Signal out(ch);
    for (std::uint32_t done = 0; done < total;) {
      const std::uint32_t n = std::min(block, total - done);
      Call(src, &out, n, speed, speed, bad);
      done += n;
    }
    return out;
  }
};

Signal ToneSig(std::uint32_t ch, std::uint32_t frames, std::vector<double> hz, double amp = 0.5,
               std::uint32_t rate = kRate) {
  Signal s(ch, std::vector<float>(frames, 0.0f));
  for (std::uint32_t c = 0; c < ch; ++c)
    for (std::uint32_t i = 0; i < frames; ++i) {
      double v = 0.0;
      for (double f : hz) v += amp * std::sin(2.0 * kPi * f * i / rate);
      s[c][i] = static_cast<float>(v);
    }
  return s;
}

Signal NoiseSig(std::uint32_t ch, std::uint32_t frames, std::uint32_t seed, float amp = 0.5f) {
  Signal s(ch, std::vector<float>(frames));
  for (std::uint32_t c = 0; c < ch; ++c) {
    dsptest::Noise nz(seed + 31u * c);
    for (float& v : s[c]) v = nz.Next(amp);
  }
  return s;
}

// Strongest frequency in [lo, hi] with `step` Hz resolution.
double PeakHz(const float* x, std::uint32_t n, double lo, double hi, double step, std::uint32_t rate = kRate) {
  double best = lo, bestA = -1.0;
  for (double f = lo; f <= hi; f += step) {
    const double a = dsptest::ToneAmplitude(x, n, f, rate);
    if (a > bestA) {
      bestA = a;
      best = f;
    }
  }
  return best;
}

// Envelope of a sinusoid: amplitude estimated per window of `w` frames
// (sqrt(2) * RMS). Returns min and max over [from, to).
void Envelope(const std::vector<float>& x, std::uint32_t from, std::uint32_t to, std::uint32_t w,
              double* mn, double* mx) {
  *mn = 1e9;
  *mx = 0.0;
  for (std::uint32_t i = from; i + w <= to; i += w / 2u) {
    const double a = std::sqrt(2.0) * dsptest::Rms(x.data() + i, w);
    *mn = std::min(*mn, a);
    *mx = std::max(*mx, a);
  }
}

// Total harmonic+noise residual of a sinusoid at f: fits a*sin + b*cos + c per
// window of w frames and returns the worst residual RMS relative to the fitted amplitude.
double WorstResidual(const std::vector<float>& x, std::uint32_t from, std::uint32_t to, double f,
                     std::uint32_t w) {
  double worst = 0.0;
  for (std::uint32_t i = from; i + w <= to; i += w) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (std::uint32_t j = 0; j < w; ++j) {
      const double a = 2.0 * kPi * f * (i + j) / kRate;
      const double s = std::sin(a), c = std::cos(a);
      ss += s * s;
      cc += c * c;
      sc += s * c;
      xs += x[i + j] * s;
      xc += x[i + j] * c;
    }
    const double det = ss * cc - sc * sc;
    const double A = (xs * cc - xc * sc) / det, B = (xc * ss - xs * sc) / det;
    double r = 0.0;
    for (std::uint32_t j = 0; j < w; ++j) {
      const double a = 2.0 * kPi * f * (i + j) / kRate;
      const double d = x[i + j] - (A * std::sin(a) + B * std::cos(a));
      r += d * d;
    }
    const double amp = std::hypot(A, B);
    worst = std::max(worst, std::sqrt(2.0 * r / w) / (amp > 1e-9 ? amp : 1e-9));
  }
  return worst;
}

std::uint32_t RoundUp16(std::uint32_t v) { return (v + 15u) & ~15u; }

}  // namespace

// T1: the shape formulas for many rates, max speeds, windows and search radii,
// including the clamping of every argument; invalid rates / channels.
SND_TEST(IndepTimeStretch, ShapeFormulas) {
  const std::uint32_t rates[] = {1u, 1000u, 8000u, 11025u, 22050u, 44100u, 48000u, 96000u, 192000u, 384000u};
  // NaN maxSpeed maps to 1 (D-217).
  const float speeds[] = {1.0f, 1.5f, 2.5f, 4.0f, 0.5f, 7.0f};
  const float wins[] = {30.0f, 5.0f, 1.0f, 200.0f, 500.0f, 47.3f, std::nanf("")};
  const float searches[] = {8.0f, 0.0f, -3.0f, 50.0f, 80.0f, 2.7f, std::nanf("")};
  for (std::uint32_t r : rates)
    for (float ms : speeds)
      for (float w : wins)
        for (float sr : searches) {
          TimeStretchShape s{};
          SND_REQUIRE(TimeStretchGetShape(r, ms, &s, w, sr));
          const double wc = std::isnan(w) ? 30.0 : std::clamp(static_cast<double>(w), 5.0, 200.0);
          const double sc = std::isnan(sr) ? 12.0 : std::clamp(static_cast<double>(sr), 0.0, 50.0);
          // D-210: maxSpeed NaN is 1.
          const double mc = std::isnan(ms) ? 1.0 : std::clamp(static_cast<double>(ms), 1.0, 4.0);
          std::uint32_t H = static_cast<std::uint32_t>(std::lround(r * wc / 2000.0));
          if (H < 16u) H = 16u;
          const std::uint32_t S = static_cast<std::uint32_t>(std::lround(r * sc / 1000.0));
          std::uint32_t D = 1;
          while (D * 2u <= std::max(1u, H / 64u)) D *= 2u;
          const std::uint32_t pad = RoundUp16(2u * S + H + static_cast<std::uint32_t>(std::ceil(mc * H)) + 2u);
          const bool okShape = s.hopFrames == H && s.searchFrames == S && s.decimation == D &&
                               s.lookaheadFrames == S + 2u * H && s.padFrames == pad && s.padFrames % 16u == 0u;
          if (!okShape) {
            SND_CHECK_MSG(false,
                          "rate %u maxSpeed %g win %g search %g: got H %u S %u D %u LA %u pad %u, want %u %u %u %u %u",
                          r, ms, w, sr, s.hopFrames, s.searchFrames, s.decimation, s.lookaheadFrames, s.padFrames,
                          H, S, D, S + 2u * H, pad);
            return;
          }
          const std::size_t b = TimeStretchStateBytes(2, r, ms, w, sr);
          SND_CHECK(b > 0u && b % 64u == 0u);
        }
  TimeStretchShape s{};
  SND_CHECK(TimeStretchGetShape(48000, 4.0f, &s));
  SND_CHECK_EQ(s.hopFrames, 720u);
  SND_CHECK_EQ(s.searchFrames, 576u);
  SND_CHECK_EQ(s.decimation, 8u);
  SND_CHECK_EQ(s.lookaheadFrames, 2016u);
  SND_CHECK(!TimeStretchGetShape(0, 1.0f, &s));
  SND_CHECK(!TimeStretchGetShape(384001, 1.0f, &s));
  SND_CHECK(!TimeStretchGetShape(48000, 1.0f, nullptr));
  for (std::uint32_t ch : {0u, 9u, 100u}) SND_CHECK_EQ(TimeStretchStateBytes(ch, 48000), 0u);
  SND_CHECK_EQ(TimeStretchStateBytes(2, 0), 0u);
  SND_CHECK_EQ(TimeStretchStateBytes(2, 384001), 0u);
  std::vector<unsigned char> m(1u << 20, 0x5A);
  SND_CHECK(!TimeStretchInit(m.data(), m.size(), 0, 48000));
  SND_CHECK(!TimeStretchInit(m.data(), m.size(), 9, 48000));
  SND_CHECK(!TimeStretchInit(m.data(), m.size(), 2, 0));
  SND_CHECK(!TimeStretchInit(nullptr, m.size(), 2, 48000));
  const std::size_t b = TimeStretchStateBytes(2, 48000);
  SND_CHECK(!TimeStretchInit(m.data(), b - 1u, 2, 48000));
  bool untouched = true;
  for (unsigned char c : m) untouched = untouched && c == 0x5A;
  SND_CHECK_MSG(untouched, "a failed Init wrote to the state");
  // D-217 specifies these two state sizes.
  snd::test::Note("state bytes: stereo 48k x4 %zu, 8ch 96k x4 %zu, 8ch 384k x4 %zu", b,
                  TimeStretchStateBytes(8, 96000), TimeStretchStateBytes(8, 384000));
  SND_CHECK_EQ(b, 41152u);
  SND_CHECK_EQ(TimeStretchStateBytes(8, 96000), 310528u);
}

// T2 / T15: state, input and output guards over a long run with every kind of
// speed change, on 1..8 channels and rates 8 kHz .. 384 kHz; the input frames
// behind the pad are never written; Init writes nothing past StateBytes.
SND_TEST(IndepTimeStretch, GuardsAndRates) {
  for (std::uint32_t rate : {8000u, 22050u, 44100u, 48000u, 96000u, 192000u, 384000u}) {
    for (std::uint32_t ch : {1u, 2u, 8u}) {
      for (float maxSpeed : {1.0f, 2.0f, 4.0f}) {
        Ts ts(ch, rate, maxSpeed);
        SND_REQUIRE(ts.ok);
        SND_CHECK(ts.GuardIntact());
        const Signal src = NoiseSig(ch, rate * 3u, rate + ch);
        Signal out(ch);
        int bad = 0;
        dsptest::Noise nz(rate);
        const std::uint32_t total = rate / 2u;  // 0.5 s of output
        for (std::uint32_t done = 0; done < total;) {
          const std::uint32_t n = std::min<std::uint32_t>(1u + static_cast<std::uint32_t>((nz.Next(1.0f) + 1.0f) * 400.0f), total - done);
          const float s0 = 0.25f + (nz.Next(1.0f) + 1.0f) * 2.0f, s1 = 0.25f + (nz.Next(1.0f) + 1.0f) * 2.0f;
          SND_CHECK(TimeStretchInputFrames(ts.st(), n, s0, s1) <= TimeStretchMaxInputFrames(ts.st(), n));
          ts.Call(src, &out, n, s0, s1, &bad);
          done += n;
        }
        SND_CHECK_MSG(bad == 0, "rate %u ch %u: %d buffer violations", rate, ch, bad);
        SND_CHECK_MSG(ts.GuardIntact(), "rate %u ch %u: state written past StateBytes", rate, ch);
        bool finite = true;
        for (auto& c : out)
          for (float v : c) finite = finite && std::isfinite(v) && std::fabs(v) <= 0.5f + 1e-5f;
        SND_CHECK_MSG(finite, "rate %u ch %u: output out of the input range", rate, ch);
      }
    }
  }
}

// T5: InputFrames is pure; Process consumes exactly that; too little input
// leaves the state unchanged and outputs silence; too much input consumes only
// the requirement and gives the same output as the exact amount.
SND_TEST(IndepTimeStretch, InputRequirementContract) {
  const Signal src = NoiseSig(2, 48000 * 4, 9);
  Ts a(2), b(2);
  Signal oa(2), ob(2);
  for (int i = 0; i < 7; ++i) {
    a.Call(src, &oa, 333, 1.7f, 0.6f);
    b.Call(src, &ob, 333, 1.7f, 0.6f);
  }
  std::vector<unsigned char> before(a.p, a.p + a.bytes);
  const std::uint32_t n1 = TimeStretchInputFrames(a.st(), 2000, 2.3f, 2.3f);
  const std::uint32_t n2 = TimeStretchInputFrames(a.st(), 2000, 2.3f, 2.3f);
  SND_CHECK_EQ(n1, n2);
  SND_REQUIRE(n1 > 1u);
  SND_CHECK(std::memcmp(before.data(), a.p, a.bytes) == 0);
  const std::uint32_t pad = TimeStretchPadFrames(a.st());
  // Too little.
  {
    std::vector<std::vector<float>> in(2, std::vector<float>(pad + n1, 0.1f)), out(2, std::vector<float>(2000, 9.0f));
    float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    std::uint32_t r;
    {
      SND_RT_SCOPE();
      r = TimeStretchProcess(a.st(), ip, n1 - 1u, op, 2000, 2.3f, 2.3f);
    }
    SND_CHECK_EQ(r, 0u);
    bool silent = true;
    for (auto& c : out)
      for (float v : c) silent = silent && v == 0.0f;
    SND_CHECK(silent);
    SND_CHECK_MSG(std::memcmp(before.data(), a.p, a.bytes) == 0, "short input changed the state");
    r = TimeStretchProcess(a.st(), ip, 0, op, 2000, 2.3f, 2.3f);
    SND_CHECK_EQ(r, 0u);
    SND_CHECK(std::memcmp(before.data(), a.p, a.bytes) == 0);
  }
  // Too much: 1000 extra frames.
  {
    std::vector<std::vector<float>> in(2, std::vector<float>(pad + n1 + 1000u)), out(2, std::vector<float>(2000));
    for (std::uint32_t c = 0; c < 2; ++c)
      for (std::uint32_t i = 0; i < n1 + 1000u; ++i) in[c][pad + i] = src[c][a.pos + i];
    float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    const std::uint32_t r = TimeStretchProcess(a.st(), ip, n1 + 1000u, op, 2000, 2.3f, 2.3f);
    SND_CHECK_EQ(r, n1);
    a.pos += r;
    Signal tail(2);
    for (std::uint32_t c = 0; c < 2; ++c) tail[c] = out[c];
    Signal bo(2);
    b.Call(src, &bo, 2000, 2.3f, 2.3f);
    SND_CHECK(bo[0] == tail[0] && bo[1] == tail[1]);
    SND_CHECK_EQ(TimeStretchConsumedFrames(a.st()), TimeStretchConsumedFrames(b.st()));
    // Both continue identically.
    Signal x(2), y(2);
    a.Call(src, &x, 2000, 0.8f, 0.8f);
    b.Call(src, &y, 2000, 0.8f, 0.8f);
    SND_CHECK(x[0] == y[0] && x[1] == y[1]);
  }
  // Zero output frames: nothing needed, nothing changes.
  std::vector<unsigned char> s2(a.p, a.p + a.bytes);
  SND_CHECK_EQ(TimeStretchInputFrames(a.st(), 0, 1.0f, 1.0f), 0u);
  float* none[2] = {nullptr, nullptr};
  std::vector<float> pb(pad);
  float* ip[2] = {pb.data(), pb.data()};
  SND_CHECK_EQ(TimeStretchProcess(a.st(), ip, 0, none, 0, 1.0f, 1.0f), 0u);
  (void)s2;
}

// T6: long runs. consumed = speed * frames + look-ahead within one hop of input
// advance, at 2 s and at 20 s (no drift), for constant speeds including
// non-dyadic ones. Data independent: the same with silence and with noise.
SND_TEST(IndepTimeStretch, LongRunConsumptionNoDrift) {
  const Signal zero(1, std::vector<float>());  // reads as zeros
  for (float speed : {0.25f, 0.5f, 0.7f, 1.0f, 1.1f, 2.0f, 3.3f, 4.0f}) {
    Ts ts(1);
    const double H = 720.0, LA = 2016.0;
    double dev2 = 0.0, dev20 = 0.0;
    std::uint64_t total = 0;
    Signal out(1);
    for (std::uint32_t sec = 1; sec <= 20; ++sec) {
      for (std::uint32_t i = 0; i < 48000u / 256u + (sec % 4 == 0 ? 1u : 0u); ++i) {
        ts.Call(zero, &out, 256, speed, speed);
        total += 256u;
        out[0].clear();
      }
      const double want = static_cast<double>(speed) * total + LA;
      const double dev = static_cast<double>(TimeStretchConsumedFrames(ts.st())) - want;
      if (sec == 2) dev2 = dev;
      if (sec == 20) dev20 = dev;
      SND_CHECK_MSG(std::fabs(dev) <= static_cast<double>(speed) * H + 2.0,
                    "speed %g after %llu frames: consumed - (speed*frames + LA) = %g", speed,
                    static_cast<unsigned long long>(total), dev);
    }
    // Drift: the deviation is a function of the phase inside the hop only.
    SND_CHECK_MSG(std::fabs(dev20 - dev2) <= static_cast<double>(speed) * H + 2.0, "speed %g drift %g -> %g", speed,
                  dev2, dev20);
  }
}

// T8: speed 1 from Init is the input bit for bit with no delay, for 8 channels,
// odd block sizes and rates 8 kHz / 192 kHz; Reset restores that.
SND_TEST(IndepTimeStretch, SpeedOneBitExact) {
  for (std::uint32_t rate : {8000u, 48000u, 192000u}) {
    const Signal src = NoiseSig(8, rate * 2u, 4u);
    Ts ts(8, rate);
    for (int pass = 0; pass < 2; ++pass) {
      Signal out(8);
      const std::uint32_t sizes[] = {1u, 7u, 256u, 1000u, 13u, 4096u};
      std::uint32_t done = 0, k = 0;
      while (done < rate) {
        const std::uint32_t n = std::min(sizes[k++ % 6u], rate - done);
        ts.Call(src, &out, n, 1.0f, 1.0f);
        SND_CHECK(TimeStretchPendingOffset(ts.st()) <= 0.0);
        done += n;
      }
      bool same = true;
      for (std::uint32_t c = 0; c < 8; ++c)
        same = same && std::memcmp(out[c].data(), src[c].data(), 4u * rate) == 0;
      SND_CHECK_MSG(same, "rate %u pass %d: speed 1 is not the input", rate, pass);
      // Next output frame is source frame `rate`.
      SND_CHECK_EQ(static_cast<double>(TimeStretchConsumedFrames(ts.st())) + TimeStretchPendingOffset(ts.st()),
                   static_cast<double>(rate));
      {
        SND_RT_SCOPE();
        TimeStretchReset(ts.st());
      }
      ts.pos = 0;
      SND_CHECK_EQ(TimeStretchConsumedFrames(ts.st()), 0u);
      SND_CHECK_EQ(TimeStretchPendingOffset(ts.st()), 0.0);
    }
  }
}

// T8 / T13: after other speeds, speed 1 gives the input delayed by a constant:
// from two hops on, out[j] = x[P + j] bit for bit with P = consumed + offset,
// and |P - integral of the speed| <= S.
SND_TEST(IndepTimeStretch, BackToSpeedOneIsAConstantOffset) {
  const Signal src = NoiseSig(2, 48000 * 8, 21);
  for (float speed : {0.3f, 0.77f, 1.5f, 2.9f}) {
    Ts ts(2);
    Signal out(2);
    for (int i = 0; i < 150; ++i) {
      ts.Call(src, &out, 256, speed, speed);
    }
    for (int i = 0; i < 6; ++i) {  // > 2 hops at speed 1
      ts.Call(src, &out, 256, 1.0f, 1.0f);
    }
    const double P = static_cast<double>(TimeStretchConsumedFrames(ts.st())) + TimeStretchPendingOffset(ts.st());
    const std::int64_t p = static_cast<std::int64_t>(P);
    SND_CHECK_EQ(static_cast<double>(p), P);
    // The first hop starts at zero. Hop boundaries through frame 38400
    // use the initial speed, then use 1. The selected position is within S
    // of the integer nominal position, including the current partial hop.
    constexpr int initialHops = (150 * 256 - 1) / 720;
    const double nominal = std::floor(initialHops * 720.0 * speed) +
                           static_cast<double>(out[0].size()) - initialHops * 720.0;
    SND_CHECK_MSG(std::fabs(P - nominal) <= 576.0, "speed %g: offset %g", speed, P - nominal);
    Signal tail(2);
    const std::size_t from = out[0].size();
    for (int i = 0; i < 100; ++i) ts.Call(src, &out, 256, 1.0f, 1.0f);
    bool exact = true;
    for (std::uint32_t c = 0; c < 2; ++c)
      exact = exact && std::memcmp(out[c].data() + from, src[c].data() + p, 4u * 25600u) == 0;
    SND_CHECK_MSG(exact, "speed %g then 1: not the input at a constant offset (P = %g)", speed, P);
  }
}

// T3: out-of-range speeds behave as the clamped value: NaN -> 1, 0 / negative /
// tiny -> 0.25, +Inf / huge -> maxSpeed (also for an instance with maxSpeed 2).
SND_TEST(IndepTimeStretch, SpeedClamping) {
  const Signal src = NoiseSig(1, 48000 * 6, 3);
  struct Case {
    float given, same;
    float maxSpeed;
  };
  const float inf = INFINITY;
  const Case cases[] = {{std::nanf(""), 1.0f, 4.0f}, {0.0f, 0.25f, 4.0f}, {-2.0f, 0.25f, 4.0f}, {0.01f, 0.25f, 4.0f},
                        {-inf, 0.25f, 4.0f},         {inf, 4.0f, 4.0f},   {100.0f, 4.0f, 4.0f}, {3.0f, 2.0f, 2.0f},
                        {inf, 2.0f, 2.0f},           {0.1f, 0.25f, 1.0f}, {2.0f, 1.0f, 1.0f}};
  for (const Case& cs : cases) {
    Ts a(1, kRate, cs.maxSpeed), b(1, kRate, cs.maxSpeed);
    const Signal x = a.Run(src, 24000, 256, cs.given);
    const Signal y = b.Run(src, 24000, 256, cs.same);
    SND_CHECK_MSG(x[0] == y[0] && TimeStretchConsumedFrames(a.st()) == TimeStretchConsumedFrames(b.st()),
                  "speed %g (max %g) does not behave as %g", cs.given, cs.maxSpeed, cs.same);
  }
}

// T9: constant speed: bit-identical for any block split (1, prime, 256, 4096,
// irregular). Ramps split at the matching intermediate speeds: equal up to the
// float rounding of speed(i) (reported).
SND_TEST(IndepTimeStretch, BlockSplitting) {
  const Signal src = NoiseSig(2, 48000 * 8, 5);
  for (float speed : {0.37f, 1.0f, 1.61f, 3.9f}) {
    Ts ref(2);
    const Signal r = ref.Run(src, 48000, 48000, speed);
    for (std::uint32_t block : {1u, 7u, 97u, 256u, 4096u}) {
      Ts t(2);
      const Signal o = t.Run(src, 48000, block, speed);
      SND_CHECK_MSG(o[0] == r[0] && o[1] == r[1], "speed %g block %u differs", speed, block);
    }
  }
  // Ramp 0.5 -> 2.5 over 4800 frames, as one call and as 3 calls (1000, 2000, 1800).
  Ts a(1), b(1);
  Signal oa(1), ob(1);
  const Signal mono(1, src[0]);
  a.Call(mono, &oa, 4800, 0.5f, 2.5f);
  const auto sp = [](double i) { return static_cast<float>(0.5 + 2.0 * i / 4800.0); };
  b.Call(mono, &ob, 1000, 0.5f, sp(1000));
  b.Call(mono, &ob, 2000, sp(1000), sp(3000));
  b.Call(mono, &ob, 1800, sp(3000), 2.5f);
  const float diff = dsptest::MaxAbsDiff(oa[0].data(), ob[0].data(), 4800);
  const std::int64_t dc = static_cast<std::int64_t>(TimeStretchConsumedFrames(a.st())) -
                          static_cast<std::int64_t>(TimeStretchConsumedFrames(b.st()));
  snd::test::Note("ramp split in 3: max diff %g, consumed diff %lld", static_cast<double>(diff), static_cast<long long>(dc));
  SND_CHECK(std::llabs(dc) <= 1);
}

// T4 / T6: ramps against an own simulation of the hop rule in double: hop k
// starts at output frame i_k (k H minus the frames already output), uses
// speed(i_k) = s0 + (s1 - s0) i_k / n, tau += round(speed H 2^32), the
// requirement is floor(tau) + S + 2H.
SND_TEST(IndepTimeStretch, RampFollowsOwnSimulation) {
  const std::uint32_t H = 720, S = 576;
  struct Seg {
    std::uint32_t n;
    float s0, s1;
  };
  const Seg segs[] = {{1000, 0.5f, 2.0f}, {256, 2.0f, 4.0f}, {5000, 4.0f, 0.25f}, {333, 0.25f, 0.25f},
                      {720, 1.0f, 3.0f},  {1, 3.0f, 3.0f},   {2160, 1.3f, 0.9f}};
  Ts ts(1);
  const Signal zero(1, std::vector<float>());
  Signal out(1);
  // Simulation state.
  bool started = false;
  std::uint32_t h = 0;
  std::uint64_t tau = 0;
  std::int64_t req = 0;
  for (int rep = 0; rep < 5; ++rep)
    for (const Seg& sg : segs) {
      for (std::uint32_t o = 0; o < sg.n;) {
        if (!started || h >= H) {
          if (!started) {
            started = true;
            tau = 0;
          } else {
            double v = sg.s0 + (static_cast<double>(sg.s1) - sg.s0) * o / sg.n;
            v = std::clamp(v, 0.25, 4.0);
            tau += static_cast<std::uint64_t>(std::llround(v * H * 4294967296.0));
          }
          req = std::max<std::int64_t>(req, static_cast<std::int64_t>(tau >> 32) + S + 2 * H);
          h = 0;
        }
        const std::uint32_t len = std::min(H - h, sg.n - o);
        h += len;
        o += len;
      }
      ts.Call(zero, &out, sg.n, sg.s0, sg.s1);
      const std::int64_t got = static_cast<std::int64_t>(TimeStretchConsumedFrames(ts.st()));
      SND_CHECK_MSG(std::llabs(got - req) <= 1, "segment %u frames %g->%g: consumed %lld, simulated %lld", sg.n,
                    sg.s0, sg.s1, static_cast<long long>(got), static_cast<long long>(req));
    }
}

// T11: pitch and amplitude of sinusoids from 50 Hz to 10 kHz at speeds 0.25 .. 4.
// The peak stays within 1 Hz (1 s DFT at 0.1 Hz steps around it). The amplitude
// envelope (per 10 ms) and the worst residual against a fitted sinusoid are
// reported; the envelope must stay within -3 dB .. +0.5 dB for 200 Hz and up.
SND_TEST(IndepTimeStretch, SinusoidPitchAndAmplitude) {
  for (double hz : {50.0, 55.0, 73.0, 100.0, 220.0, 440.0, 1000.0, 3150.0, 10000.0}) {
    const Signal src = ToneSig(1, 48000 * 9, {hz});
    for (float speed : {0.25f, 0.5f, 0.8f, 1.5f, 2.0f, 4.0f}) {
      Ts ts(1);
      const Signal o = ts.Run(src, 48000 * 2, 256, speed);
      const double peak = PeakHz(o[0].data() + 24000, 48000, hz - 3.0, hz + 3.0, 0.1);
      SND_CHECK_MSG(std::fabs(peak - hz) <= 1.0, "%g Hz at speed %g: peak %g Hz", hz, speed, peak);
      double mn, mx;
      Envelope(o[0], 9600, 96000, 480, &mn, &mx);
      const double res = WorstResidual(o[0], 9600, 96000, hz, 960);
      snd::test::Note("%6.0f Hz x%.2f: envelope %+.2f .. %+.2f dB, worst residual %.3f", hz, speed,
                      dsptest::Db(mn / 0.5), dsptest::Db(mx / 0.5), res);
      if (hz <= 220.0 && speed <= 2.0f) {
        // Phase mismatch of at most one sample on this tone bounds the
        // difference between it and a convex mix with its shifted copy.
        const double phaseStep = 2.0 * std::sin(kPi * hz / kRate);
        SND_CHECK_MSG(res <= phaseStep, "%g Hz x%g: residual %g", hz, speed, res);
      }
      if (hz >= 200.0) {
        SND_CHECK_MSG(dsptest::Db(mn / 0.5) > -3.0 && dsptest::Db(mx / 0.5) < 0.5,
                      "%g Hz at speed %g: envelope %g .. %g dB", hz, speed, dsptest::Db(mn / 0.5),
                      dsptest::Db(mx / 0.5));
      }
    }
  }
  // Two tones at once keep both frequencies.
  const Signal two = ToneSig(1, 48000 * 6, {440.0, 660.0}, 0.3);
  for (float speed : {0.5f, 2.0f}) {
    Ts ts(1);
    const Signal o = ts.Run(two, 48000 * 2, 256, speed);
    const double a440 = dsptest::ToneAmplitude(o[0].data() + 24000, 48000, 440.0, kRate);
    const double a660 = dsptest::ToneAmplitude(o[0].data() + 24000, 48000, 660.0, kRate);
    const double a550 = dsptest::ToneAmplitude(o[0].data() + 24000, 48000, 550.0, kRate);
    snd::test::Note("440+660 x%.1f: %.3f %.3f (550 Hz: %.4f)", speed, a440, a660, a550);
    SND_CHECK(a440 > 0.2 && a660 > 0.2 && a550 < 0.03);
  }
}

// T12: transients. A click train (one click every 100 ms): reports, per input
// click, whether it is missing (< 0.25 in the output), weak (< 0.7) or doubled
// (two output samples >= 0.25 near its place) and the worst shift from c / speed.
// The spec promises nothing explicit here; speed 1 must be exact. The other
// speeds are reported (see the report: WSOLA doubles clicks below speed 1 and
// drops a third of them at speed 2).
SND_TEST(IndepTimeStretch, Transients) {
  const std::uint32_t spacing = 4800, clicks = 40;
  Signal src(1, std::vector<float>(spacing * clicks + 48000, 0.0f));
  for (std::uint32_t k = 0; k < clicks; ++k) src[0][spacing / 2u + k * spacing] = 1.0f;
  for (float speed : {0.5f, 0.8f, 1.0f, 1.5f, 2.0f, 4.0f}) {
    Ts ts(1);
    const std::uint32_t outFrames = static_cast<std::uint32_t>((spacing * clicks) / speed);
    const Signal o = ts.Run(src, outFrames, 256, speed);
    std::uint32_t missing = 0, doubled = 0, weak = 0, worstShift = 0;
    for (std::uint32_t k = 0; k + 1u < clicks; ++k) {
      const double c = (spacing / 2u + k * spacing) / static_cast<double>(speed);
      const std::uint32_t lo = static_cast<std::uint32_t>(std::max(0.0, c - 0.45 * spacing / speed));
      const std::uint32_t hi = static_cast<std::uint32_t>(std::min<double>(outFrames, c + 0.45 * spacing / speed));
      std::uint32_t strong = 0;
      float best = 0.0f;
      std::uint32_t bestAt = lo;
      for (std::uint32_t i = lo; i < hi; ++i) {
        const float v = std::fabs(o[0][i]);
        if (v >= 0.25f) ++strong;
        if (v > best) {
          best = v;
          bestAt = i;
        }
      }
      if (best < 0.25f) ++missing;
      else if (best < 0.7f) ++weak;
      if (strong >= 2u) ++doubled;
      if (best >= 0.25f) worstShift = std::max(worstShift, static_cast<std::uint32_t>(std::fabs(bestAt - c)));
    }
    snd::test::Note("clicks x%.1f: missing %u, weak (0.25..0.7) %u, doubled %u of %u, worst shift %u frames", speed,
                    missing, weak, doubled, clicks - 1u, worstShift);
    if (speed == 1.0f) SND_CHECK(missing == 0u && weak == 0u && doubled == 0u && worstShift == 0u);
    if (speed < 1.0f) SND_CHECK(missing == 0u);
    SND_CHECK(worstShift <= 2u * (576u + 720u));
  }
}

// T10: channel coherence. 8 identical channels give identical bits; positive
// power-of-two scales and zero channels scale exactly; an arbitrary phase
// difference (37 deg at 1 kHz and 3 kHz) is preserved.
SND_TEST(IndepTimeStretch, ChannelCoherence) {
  const Signal mono = NoiseSig(1, 48000 * 6, 77);
  Signal src(8, mono[0]);
  const float scale[8] = {1.0f, 1.0f, 0.5f, 0.25f, 2.0f, 0.0f, 1.0f, 0.125f};
  for (std::uint32_t c = 0; c < 8; ++c)
    for (float& v : src[c]) v *= scale[c];
  for (float speed : {0.6f, 1.4f, 3.1f}) {
    Ts ts(8);
    const Signal o = ts.Run(src, 48000, 256, speed);
    bool ok = true;
    for (std::uint32_t c = 1; c < 8; ++c)
      for (std::size_t i = 0; i < o[0].size(); ++i) ok = ok && o[c][i] == o[0][i] * scale[c];
    SND_CHECK_MSG(ok, "speed %g: channels are not exact scaled copies", speed);
  }
  for (double hz : {1000.0, 3000.0}) {
    const double ph = 37.0 * kPi / 180.0;
    Signal st(2, std::vector<float>(48000 * 6));
    for (std::uint32_t i = 0; i < st[0].size(); ++i) {
      st[0][i] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / kRate));
      st[1][i] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / kRate + ph));
    }
    for (float speed : {0.5f, 1.7f}) {
      Ts ts(2);
      const Signal o = ts.Run(st, 48000 * 2, 256, speed);
      double worst = 0.0;
      for (std::uint32_t w = 9600; w + 960 <= 96000; w += 960) {
        double ar = 0, ai = 0, br = 0, bi = 0;
        for (std::uint32_t j = 0; j < 960; ++j) {
          const double a = 2 * kPi * hz * (w + j) / kRate;
          ar += o[0][w + j] * std::cos(a);
          ai += o[0][w + j] * std::sin(a);
          br += o[1][w + j] * std::cos(a);
          bi += o[1][w + j] * std::sin(a);
        }
        double d = std::atan2(bi, br) - std::atan2(ai, ar);
        while (d > kPi) d -= 2 * kPi;
        while (d < -kPi) d += 2 * kPi;
        worst = std::max(worst, std::fabs(std::fabs(d) - ph));
      }
      SND_CHECK_MSG(worst < 0.02, "%g Hz speed %g: phase difference off by %g rad", hz, speed, worst);
    }
  }
}

// T16: silence -> exact zeros; DC stays DC (sin^2 + cos^2 = 1); full scale never
// overshoots; denormal input stays finite and tiny; NaN input recovers once it
// has left the history.
SND_TEST(IndepTimeStretch, SpecialSignals) {
  for (float speed : {0.5f, 1.3f, 3.7f}) {
    {
      const Signal z(2, std::vector<float>(48000 * 3, 0.0f));
      Ts ts(2);
      const Signal o = ts.Run(z, 48000, 256, speed);
      bool zero = true;
      for (auto& c : o)
        for (float v : c) zero = zero && v == 0.0f;
      SND_CHECK(zero);
    }
    {
      const Signal dc(1, std::vector<float>(48000 * 5, 0.75f));
      Ts ts(1);
      const Signal o = ts.Run(dc, 48000, 256, speed);
      float dev = 0.0f;
      for (float v : o[0]) dev = std::max(dev, std::fabs(v - 0.75f));
      SND_CHECK_MSG(dev < 1e-6f, "DC at speed %g deviates by %g", speed, static_cast<double>(dev));
    }
    {
      Signal fs(1, std::vector<float>(48000 * 5));
      for (std::uint32_t i = 0; i < fs[0].size(); ++i) fs[0][i] = (i / 37u) % 2u ? 1.0f : -1.0f;
      Ts ts(1);
      const Signal o = ts.Run(fs, 48000, 256, speed);
      float mx = 0.0f;
      for (float v : o[0]) mx = std::max(mx, std::fabs(v));
      SND_CHECK_MSG(mx <= 1.0f + 1e-6f, "full scale overshoots to %g", static_cast<double>(mx));
    }
    {
      Signal dn(1, std::vector<float>(48000 * 5));
      dsptest::Noise nz(1);
      for (float& v : dn[0]) v = nz.Next(1.0f) * 1e-39f;
      Ts ts(1);
      const Signal o = ts.Run(dn, 48000, 256, speed);
      bool ok = true;
      for (float v : o[0]) ok = ok && std::isfinite(v) && std::fabs(v) < 1e-38f;
      SND_CHECK(ok);
    }
    {
      Signal nn = NoiseSig(1, 48000 * 10, 8);
      for (std::uint32_t i = 10000; i < 10100; ++i) nn[0][i] = std::nanf("");
      Ts ts(1);
      const Signal o = ts.Run(nn, static_cast<std::uint32_t>(48000 * 2 / speed) + 48000, 256, speed);
      std::size_t lastBad = 0;
      for (std::size_t i = 0; i < o[0].size(); ++i)
        if (!std::isfinite(o[0][i])) lastBad = i;
      snd::test::Note("NaN at source 10000..10100, speed %.1f: last non-finite output frame %zu", speed, lastBad);
      SND_CHECK_MSG(lastBad < 10100.0 / speed + 3000.0, "speed %g: NaN persists to output frame %zu", speed, lastBad);
    }
  }
}

// T14: Reset is a fresh Init: after any history, Reset + the same input gives
// the same bits as a new instance.
SND_TEST(IndepTimeStretch, ResetReproduces) {
  const Signal src = NoiseSig(2, 48000 * 6, 12), other = NoiseSig(2, 48000 * 6, 13);
  Ts fresh(2), used(2);
  const Signal want = fresh.Run(src, 30000, 300, 1.37f);
  used.Run(other, 20000, 111, 2.9f);
  TimeStretchReset(used.st());
  used.pos = 0;
  const Signal got = used.Run(src, 30000, 300, 1.37f);
  SND_CHECK(got[0] == want[0] && got[1] == want[1]);
  SND_CHECK_EQ(TimeStretchConsumedFrames(used.st()), TimeStretchConsumedFrames(fresh.st()));
}

// T13: the playhead (consumed + offset) stays within S + one hop of input
// advance of the integral of the speed, and the offset is <= 0.
SND_TEST(IndepTimeStretch, PlayheadTracksTheSpeedIntegral) {
  const Signal src = NoiseSig(1, 48000 * 30, 2);
  Ts ts(1);
  Signal out(1);
  double integral = 0.0, worst = 0.0;
  dsptest::Noise nz(5);
  float s = 1.0f;
  for (int i = 0; i < 1500; ++i) {
    const float s1 = std::clamp(s + nz.Next(0.3f), 0.25f, 4.0f);
    const std::uint32_t n = 64u + static_cast<std::uint32_t>((nz.Next(1.0f) + 1.0f) * 200.0f);
    ts.Call(src, &out, n, s, s1);
    for (std::uint32_t j = 0; j < n; ++j) integral += s + (static_cast<double>(s1) - s) * j / n;
    s = s1;
    out[0].clear();
    const double off = TimeStretchPendingOffset(ts.st());
    SND_CHECK(off <= 0.0);
    const double P = static_cast<double>(TimeStretchConsumedFrames(ts.st())) + off;
    worst = std::max(worst, std::fabs(P - integral));
  }
  snd::test::Note("playhead vs speed integral: worst %.1f frames", worst);
  SND_CHECK(worst <= 576.0 + 4.0 * 720.0 + 2.0);
}

// Cost (report): one stereo voice, 48 kHz, 256-frame blocks, mean and worst per block.
SND_TEST(IndepTimeStretch, CostPerBlock) {
  const Signal src = NoiseSig(2, 48000 * 40, 1);
  for (float speed : {0.5f, 1.0f, 1.3f, 2.0f, 4.0f}) {
    Ts ts(2);
    const std::uint32_t pad = TimeStretchPadFrames(ts.st());
    const std::uint32_t maxIn = TimeStretchMaxInputFrames(ts.st(), 256);
    std::vector<std::vector<float>> in(2, std::vector<float>(pad + maxIn)), out(2, std::vector<float>(256));
    float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    double sum = 0.0, worst = 0.0;
    const int blocks = 1500;
    std::uint64_t pos = 0;
    for (int b = 0; b < blocks; ++b) {
      const std::uint32_t n = TimeStretchInputFrames(ts.st(), 256, speed, speed);
      for (std::uint32_t c = 0; c < 2; ++c)
        for (std::uint32_t i = 0; i < n; ++i) in[c][pad + i] = src[c][(pos + i) % src[c].size()];
      const auto t0 = std::chrono::steady_clock::now();
      const std::uint32_t used = TimeStretchProcess(ts.st(), ip, n, op, 256, speed, speed);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      pos += used;
      sum += us;
      worst = std::max(worst, us);
    }
    snd::test::Note("time stretch stereo 256 frames x%.1f: mean %.2f us, worst %.2f us (peak/mean %.1f)", speed,
                    sum / blocks, worst, worst / (sum / blocks));
  }
}

// S1 / design 2.5: the stretcher has no SIMD path; the same input must give the
// same bits in every configuration (scalar executable, -mavx2 -mfma, Release,
// sanitizers). Checksum computed once (gcc Debug).
SND_TEST(IndepTimeStretch, BitExactChecksumAcrossConfigurations) {
  const Signal src = NoiseSig(2, 48000 * 6, 4242);
  std::uint64_t h = 1469598103934665603ull;
  for (float speed : {0.43f, 1.37f, 3.1f}) {
    Ts ts(2);
    Signal out(2);
    for (int i = 0; i < 60; ++i) ts.Call(src, &out, 256, speed, speed * 1.1f > 4.0f ? 4.0f : speed * 1.1f);
    for (auto& c : out) {
      const unsigned char* b = reinterpret_cast<const unsigned char*>(c.data());
      for (std::size_t i = 0; i < c.size() * 4u; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
      }
    }
  }
  snd::test::Note("time stretch checksum %016llx", static_cast<unsigned long long>(h));
  constexpr std::uint64_t kExpected = 0xaf9b99d4a10e48eeull;
  SND_CHECK_EQ(h, kExpected);
}

SND_TEST(IndepTimeStretch, NanMaxSpeedIsOne) {
  TimeStretchShape nanShape{}, oneShape{};
  SND_REQUIRE(TimeStretchGetShape(kRate, std::nanf(""), &nanShape));
  SND_REQUIRE(TimeStretchGetShape(kRate, 1.0f, &oneShape));
  SND_CHECK_EQ(nanShape.padFrames, oneShape.padFrames);
  Ts nan(2, kRate, std::nanf("")), one(2, kRate, 1.0f);
  SND_REQUIRE(nan.ok && one.ok);
  SND_CHECK_EQ(nan.bytes, one.bytes);
  const Signal src = NoiseSig(2, 64000, 3);
  SND_CHECK(nan.Run(src, 24000, 256, 2.0f) == one.Run(src, 24000, 256, 2.0f));
}

SND_TEST(IndepTimeStretch, PerChannelCorrelation) {
  const Signal mono = ToneSig(1, 200000, {220.0});
  for (float speed : {0.7f, 1.5f}) {
    Ts reference(1);
    const auto want = reference.Run(mono, 96000, 256, speed);
    for (unsigned channels : {2u, 8u}) {
      for (bool antiPhase : {false, true}) {
        Signal src(channels, mono[0]);
        for (unsigned c = 0; c < channels; ++c) {
          const float gain = c == 0u ? 1.0f : antiPhase ? (c % 2u ? -1.0f : 1.0f) : 0.0f;
          for (float& x : src[c]) x *= gain;
        }
        Ts ts(channels);
        const auto actual = ts.Run(src, 96000, 256, speed);
        SND_CHECK(actual[0] == want[0]);
      }
    }
  }
  // At speed 4, the second hop's natural continuation falls below the
  // search window. Every candidate ties on silence, so select the first.
  Ts silent(8);
  const Signal zero(8);
  const auto out = silent.Run(zero, 721, 721, 4.0f);
  for (const auto& c : out) SND_CHECK(c == std::vector<float>(721, 0.0f));
  SND_CHECK_EQ(static_cast<double>(silent.pos) + TimeStretchPendingOffset(silent.st()), 4 * 720 - 576 + 1);
}

SND_TEST(IndepTimeStretch, ClampEndpointsBeforeInterpolation) {
  const Signal src = NoiseSig(1, 48000, 81);
  struct Endpoints { float a, b, finiteA, finiteB; };
  for (auto ramp : {Endpoints{INFINITY, -INFINITY, 2.0f, 0.25f},
                    Endpoints{-INFINITY, std::nanf(""), 0.25f, 1.0f},
                    Endpoints{std::nanf(""), INFINITY, 1.0f, 2.0f}}) {
    Ts raw(1, kRate, 2.0f), finite(1, kRate, 2.0f);
    Signal x(1), y(1);
    SND_CHECK_EQ(raw.Call(src, &x, 9000, ramp.a, ramp.b),
                 finite.Call(src, &y, 9000, ramp.finiteA, ramp.finiteB));
    SND_CHECK(x == y);
  }
}

SND_TEST(IndepTimeStretch, CallLimitSilencesOnlyTheExcess) {
  constexpr std::uint32_t limit = 1u << 20;
  Ts clipped(1), reference(1);
  SND_REQUIRE(clipped.ok && reference.ok);
  SND_CHECK_EQ(TimeStretchInputFrames(clipped.st(), UINT32_MAX, 1, 1),
               TimeStretchInputFrames(reference.st(), limit, 1, 1));
  const Signal src(1, std::vector<float>(limit + 6000u, 0.25f));
  Signal x(1), y(1);
  int bad = 0;
  SND_CHECK_EQ(clipped.Call(src, &x, limit + 7, 1, 1, &bad), reference.Call(src, &y, limit, 1, 1, &bad));
  SND_CHECK_EQ(bad, 0);
  SND_CHECK(std::all_of(y[0].begin(), y[0].end(), [](float v){ return v == 0.25f; }));
  y[0].resize(limit + 7, 0.0f);
  SND_CHECK(x == y);
  SND_CHECK(clipped.Run(src, 256, 256, 1) == reference.Run(src, 256, 256, 1));
}
