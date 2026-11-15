// Independent test: do the bus effect parameters written by buildtool reach the
// dsp effects as values, and do the effects then do what the parameters say?
// Analytic expectations only (no golden):
//  - lowpass (bank EFPV CutoffHz / Q) follows the 2nd-order low-pass magnitude
//    |H(f)| = 1 / sqrt((1 - (f/fc)^2)^2 + (f / (fc Q))^2) (bilinear warping is
//    tiny at fc = 1 kHz, 48 kHz);
//  - limiter (bank EFPV CeilingDb) holds a loud sine at the ceiling.
// The end-to-end test's limiter check (peak < -0.3 dBFS) never engages the
// limiter (its mix peaks at 0.65); this one drives it 5 dB over the ceiling.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "pipeline_util.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

namespace bt = snd::buildtool;
namespace bk = snd::bank;
namespace bf = snd::bank_format;
namespace dsp = snd::dsp;
using snd::HashId;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;

struct Chain {
  std::vector<std::vector<float>> params;
  std::vector<snd::EffectDesc> descs;
  std::vector<std::uint8_t> mem;
  std::vector<dsp::EffectSlot> slots;
  bool Init(const bk::BankView& v, const char* bus, std::uint32_t channels) {
    const std::uint32_t row = v.FindRow<bk::col::BUSS_id>(HashId(bus));
    if (row == bf::kNone) return false;
    params.reserve(8);
    descs = pipeline::BusEffectDescs(v, row, channels, kRate, kBlock, &params);
    const bf::DspPoolNeed need =
        dsp::ComputeDspPoolNeed(descs.data(), static_cast<std::uint32_t>(descs.size()), kRate, nullptr, 0);
    mem.assign(static_cast<std::size_t>(need.instanceBytes) + 64, 0);
    std::uint8_t* at = mem.data();
    while (reinterpret_cast<std::uintptr_t>(at) % 64 != 0) ++at;
    for (const snd::EffectDesc& d : descs) {
      const std::uint32_t b = dsp::EffectInstanceBytes(d, kRate, nullptr, 0);
      if (b == 0 || !dsp::EffectInit(at, b, d, kRate)) return false;
      slots.push_back(dsp::EffectSlot{d.effect_type, nullptr, at, nullptr, nullptr, 0u, 0u});
      at += b;
    }
    return true;
  }
};

// Runs `n` frames of a sine of amplitude `amp` through the chain (mono), returns the output.
std::vector<float> RunSine(Chain& c, double hz, double amp, std::size_t n) {
  std::vector<float> out(n);
  std::vector<float> buf(kBlock + 16);
  float* p = buf.data();
  while (reinterpret_cast<std::uintptr_t>(p) % 64 != 0) ++p;
  float* chans[1] = {p};
  {
    SND_RT_SCOPE();
    for (std::size_t at = 0; at < n; at += kBlock) {
      for (std::uint32_t i = 0; i < kBlock; ++i) {
        p[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(at + i) / kRate));
      }
      dsp::ProcessEffectChain(c.slots.data(), static_cast<std::uint32_t>(c.slots.size()), chans, 1, kBlock);
      for (std::uint32_t i = 0; i < kBlock && at + i < n; ++i) out[at + i] = p[i];
    }
  }
  return out;
}

double Peak(const std::vector<float>& x, std::size_t from) {
  double m = 0;
  for (std::size_t i = from; i < x.size(); ++i) m = std::fmax(m, std::fabs(x[i]));
  return m;
}
}  // namespace

SND_TEST(IndepBusEffects11, LowpassAndLimiterParametersFromTheBankAreAudible) {
  mini::Project proj("indep-bus-fx");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  proj.Add("buses/LP.json", "bus", "LP",
           "\"bank\": \"Main\", \"parent\": \"Master\", \"channels\": 1, \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [1000, 0.707]}]");
  proj.Add("buses/Lim.json", "bus", "Lim",
           "\"bank\": \"Main\", \"parent\": \"Master\", \"channels\": 1, \"effects\": [{\"type\": \"snd.fx.limiter\", \"params\": [-6, 50, 2, 0]}]");
  proj.Wav("audio/a.wav", mini::Tone(1, kRate, 480));
  proj.Add("assets/a.json", "asset", "a", "\"file\": \"audio/a.wav\"");
  proj.Add("events/E.json", "event", "E", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"a\"}]}]");
  bt::Diagnostics d;
  SND_REQUIRE_MSG(proj.Build(&d), "%s", mini::Dump(d).c_str());
  pipeline::ReadBank bank(mini::ReadBytes(proj.out() / "Main.sndb"));
  SND_REQUIRE_MSG(bank.ok, "%s", bank.error.message);
  snd::test::rt::Reset();

  // Lowpass: magnitude at several frequencies against the analytic response.
  const double fc = 1000.0, q = 0.707;
  for (double f : {250.0, 500.0, 1000.0, 2000.0, 4000.0}) {
    Chain c;
    SND_REQUIRE(c.Init(bank.view, "LP", 1));
    const std::vector<float> y = RunSine(c, f, 0.5, 48000);
    const double got = 20.0 * std::log10(Peak(y, 24000) / 0.5);
    const double r = f / fc;
    const double want = -10.0 * std::log10((1 - r * r) * (1 - r * r) + (r / q) * (r / q));
    SND_CHECK_MSG(std::fabs(got - want) < (f >= 4000.0 ? 1.0 : 0.3), "lowpass at %.0f Hz: %.2f dB, analytic %.2f dB", f,
                  got, want);
  }
  // Limiter: 0.9 (-0.9 dBFS) into a -6 dB ceiling.
  Chain lim;
  SND_REQUIRE(lim.Init(bank.view, "Lim", 1));
  const double ceiling = std::pow(10.0, -6.0 / 20.0);
  const std::vector<float> y = RunSine(lim, 220.0, 0.9, 48000);
  const double peak = Peak(y, 4800);
  SND_CHECK_MSG(peak <= ceiling * 1.01, "limiter peak %.4f above the -6 dB ceiling %.4f", peak, ceiling);
  SND_CHECK_MSG(peak >= ceiling * 0.89, "limiter output %.4f far below the ceiling %.4f (over-limiting)", peak, ceiling);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}
