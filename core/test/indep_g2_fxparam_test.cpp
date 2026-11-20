// Independent tests: runtime bus-effect parameters (D-130), compressor sidechain
// (D-131), load-LOD bypass after the tail (2.4). See indep_g2_fixture.hpp.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "indep_g2_fixture.hpp"
#include "snd/internal/commands.hpp"
#include "snd/test/rt_guard.hpp"

namespace {

using namespace g2;
using snd::HashId;

constexpr std::uint32_t kB = 256;  // block frames (indep::Config)
constexpr float kBlockSec = 256.0f / 48000.0f;

// Sfx carries an echo used as a pure gain (DryMix = param 2). E: DC 0.5 on Sfx.
struct GainRig {
  explicit GainRig(snd_config cfg = G2Config()) : h(cfg) {
    FxBank fb;
    fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, EchoGain(1.0f))};
    fb.Event("E", 500.0f);
    bank = h.Load("g", fb.Build("G"));
    h.sys->Play(HashId("E"), NoEmitter(), nullptr);
    h.Steps(4);
    level = h.LastMean(0);
  }
  float Ratio(std::size_t f) const { return h.At(f, 0) / level; }
  Fx h;
  snd_bank_handle bank{};
  float level = 0;
};

// Ramp start (first frame below 1 - eps) from `from`.
std::size_t RampStart(const GainRig& r, std::size_t from) {
  for (std::size_t f = from; f < r.h.Total(); ++f) {
    if (r.Ratio(f) < 1.0f - 1e-6f) return f;
  }
  return static_cast<std::size_t>(-1);
}

}  // namespace

// A1, A2: from the next block, linear over fade_seconds, then holds.
SND_TEST(IndepG2Param, FadeIsOneLinearRampFromTheNextBlock) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  SND_REQUIRE(r.level > 0.3f && r.level < 0.4f);  // 0.5 at the centre of a stereo pan
  const std::size_t f0 = r.h.Total();
  constexpr std::uint32_t kN = 6;  // blocks
  SND_REQUIRE_EQ(r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, kN * kBlockSec), SND_OK);
  r.h.Steps(kN + 6);
  const std::size_t s = RampStart(r, f0);
  SND_REQUIRE(s != static_cast<std::size_t>(-1));
  // Starts in the block that applies the command (or the one after: "the next block").
  SND_CHECK_MSG(s / kB == f0 / kB || s / kB == f0 / kB + 1, "ramp starts at %llu, command applied at %llu", static_cast<unsigned long long>(s), static_cast<unsigned long long>(f0));
  const std::size_t start = (s / kB) * kB;
  // The whole fade is one line: value(f) = 1 - (f - start + k) / (kN * kB), k in {0, 1}.
  double worst0 = 0, worst1 = 0;
  for (std::size_t f = start; f < start + kN * kB; ++f) {
    const double e0 = 1.0 - double(f - start) / double(kN * kB);
    const double e1 = 1.0 - double(f - start + 1) / double(kN * kB);
    worst0 = std::fmax(worst0, std::fabs(r.Ratio(f) - e0));
    worst1 = std::fmax(worst1, std::fabs(r.Ratio(f) - e1));
  }
  // One frame of the fade is 1/1536 = 6.5e-4 of the full scale (the truncation rule: FadeSecondsAreTruncatedToFramesLikeTheBusVolume).
  if (std::fmin(worst0, worst1) >= 1e-3) {
    for (std::size_t b = 0; b <= kN + 1; ++b) {
      const std::size_t f = start + b * kB;
      std::printf("  block %llu: first %.6f second %.6f last %.6f\n", static_cast<unsigned long long>(b), r.Ratio(f), r.Ratio(f + 1), r.Ratio(f + kB - 1));
    }
  }
  SND_CHECK_MSG(std::fmin(worst0, worst1) < 1e-3, "fade deviates from one line by %g / %g", worst0, worst1);
  // Reached and held.
  for (std::size_t f = start + kN * kB; f < r.h.Total(); ++f) SND_CHECK(std::fabs(r.h.At(f, 0)) < 1e-6f);
}

// Decision (re-check 1): fade seconds -> frames are truncated, as snd_set_bus_volume. With
// fade_seconds = 6 * (256 / 48000.0f) (1535.99998 frames in float) the effect parameter's fade
// takes 1535 frames, and the same fade on the bus volume has the same shape.
SND_TEST(IndepG2Param, FadeSecondsAreTruncatedToFramesLikeTheBusVolume) {
  constexpr std::uint32_t kN = 6;
  auto ramp = [](bool effect) {
    GainRig r;
    std::vector<double> v;
    if (r.h.sys == nullptr) return v;
    const std::size_t f0 = r.h.Total();
    if (effect) r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, kN * kBlockSec);
    else r.h.sys->SetBusVolume(HashId("Sfx"), 0.0f, kN * kBlockSec);
    r.h.Steps(kN + 4);
    for (std::size_t f = f0; f < r.h.Total(); ++f) v.push_back(r.Ratio(f));
    return v;
  };
  const std::vector<double> e = ramp(true), bus = ramp(false);
  SND_REQUIRE(!e.empty() && e.size() == bus.size());
  // Effect: block-end values on the line of 1535 frames (1 - 256 k / 1535), 0 from the 6th block on.
  std::size_t s = 0;
  while (s < e.size() && e[s] >= 1.0 - 1e-6) ++s;
  const std::size_t start = (s / kB) * kB;
  double worst = 0;
  for (std::uint32_t k = 0; k < kN; ++k) worst = std::fmax(worst, std::fabs(e[start + k * kB] - (1.0 - 256.0 * k / 1535.0)));
  SND_CHECK_MSG(worst < 2e-5, "block starts deviate from the 1535-frame line by %g", worst);
  SND_CHECK(std::fabs(e[start + kN * kB]) < 1e-6);
  // Same shape as the bus volume (whatever the bus's own per-block interpolation, the end is the same frame range).
  std::size_t busEnd = 0, fxEnd = 0;
  for (std::size_t f = 0; f < e.size(); ++f) {
    if (std::fabs(e[f]) > 1e-6) fxEnd = f + 1;
    if (std::fabs(bus[f]) > 1e-6) busEnd = f + 1;
  }
  SND_CHECK_MSG(fxEnd / kB == busEnd / kB, "effect fade ends in block %llu, the bus volume's in block %llu",
                static_cast<unsigned long long>(fxEnd / kB), static_cast<unsigned long long>(busEnd / kB));
}

// A3: a new target while a fade runs continues from the current value.
SND_TEST(IndepG2Param, NewTargetDuringAFadeContinuesFromTheCurrentValue) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  const std::size_t f0 = r.h.Total();
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, 8 * kBlockSec);
  r.h.Steps(4);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 1.0f, 4 * kBlockSec);
  r.h.Steps(10);
  // Steepest legal slope: from about 0.5 to 1 over 4 blocks.
  const double maxStep = 0.5 / (4.0 * kB) * 1.25 + 1e-6;
  double worst = 0;
  std::size_t at = 0;
  for (std::size_t f = f0; f + 1 < r.h.Total(); ++f) {
    const double d = std::fabs(r.Ratio(f + 1) - r.Ratio(f));
    if (d > worst) {
      worst = d;
      at = f;
    }
  }
  SND_CHECK_MSG(worst <= maxStep, "jump of %g (ratio) at frame %llu (max legal step %g)", worst, static_cast<unsigned long long>(at), maxStep);
  double lowest = 1.0;
  for (std::size_t f = f0; f < r.h.Total(); ++f) lowest = std::fmin(lowest, r.Ratio(f));
  SND_CHECK(lowest > 0.35 && lowest < 0.65);  // turned around near the middle
  SND_CHECK_NEAR(r.Ratio(r.h.Total() - 1), 1.0, 1e-5);
}

// A4: fade 0 still moves inside one block, without a step (D-26).
SND_TEST(IndepG2Param, ZeroFadeMovesInsideOneBlockWithoutAStep) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  const std::size_t f0 = r.h.Total();
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, 0.0f);
  r.h.Steps(3);
  double worst = 0;
  for (std::size_t f = f0 - 1; f + 1 < r.h.Total(); ++f) worst = std::fmax(worst, std::fabs(r.Ratio(f + 1) - r.Ratio(f)));
  SND_CHECK_MSG(worst <= 1.0 / kB * 1.01 + 1e-6, "largest step %g of the full scale (one block ramp: %g)", worst,
                1.0 / kB);
  SND_CHECK(std::fabs(r.h.At(r.h.Total() - 1, 0)) < 1e-6f);
  // Negative fade = 0 (sonus.h): back to 1 inside one block too.
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 1.0f, -5.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.Ratio(r.h.Total() - 1), 1.0, 1e-6);
}

// A5: clamped to the parameter's range.
SND_TEST(IndepG2Param, ValuesAreClampedToTheRange) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.5f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.5, 1e-5);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 7.0f, 0.0f);  // DryMix 0..1
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 1.0, 1e-5);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, -3.0f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.0, 1e-6);
  SND_CHECK(AllFinite(r.h.all));
}

// A6: argument checks at record time.
SND_TEST(IndepG2Param, RecordTimeArgumentChecks) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  auto* s = r.h.sys;
  const float nan = std::nanf(""), inf = INFINITY;
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 16, 0, 0.0f, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 0, 16, 0.0f, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 0, 2, nan, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 0, 2, inf, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, nan), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, -inf), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEffectParam(HashId("Sfx"), 15, 15, 0.0f, 0.0f), SND_OK);  // legal, unknown when applied
  r.h.Steps(3);
  // Nothing of the rejected ones was applied.
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 1.0, 1e-6);
}

// A7: unknown bus / effect / parameter: warning NOT_FOUND (object = bus id), sound unchanged.
SND_TEST(IndepG2Param, UnknownTargetsWarnNotFoundWithTheBusId) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.errors.clear();
  auto* s = r.h.sys;
  s->SetEffectParam(HashId("Nope"), 0, 2, 0.0f, 0.0f);
  r.h.Steps(2);
  s->SetEffectParam(HashId("Sfx"), 1, 2, 0.0f, 0.0f);  // Sfx has one effect
  r.h.Steps(2);
  s->SetEffectParam(HashId("Sfx"), 0, 9, 0.0f, 0.0f);  // echo has 5 parameters
  r.h.Steps(2);
  s->SetEffectParam(HashId("Master"), 0, 0, 0.0f, 0.0f);  // master has no effect
  r.h.Steps(2);
  std::uint32_t nope = 0, sfx = 0, master = 0;
  for (const Err& e : r.h.errors) {
    if (e.code != SND_ERR_NOT_FOUND) continue;
    nope += e.object == HashId("Nope") ? 1u : 0u;
    sfx += e.object == HashId("Sfx") ? 1u : 0u;
    master += e.object == HashId("Master") ? 1u : 0u;
  }
  SND_CHECK_EQ(nope, 1u);
  SND_CHECK_EQ(sfx, 2u);
  SND_CHECK_EQ(master, 1u);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 1.0, 1e-6);
}

// A8: SND_ID_NONE = the master bus's chain.
SND_TEST(IndepG2Param, NoneIsTheMasterChain) {
  Fx h(G2Config());
  FxBank fb;
  fb.b.buses[1].effects = {fb.Effect(dsp::kEffectEcho, EchoGain(1.0f))};
  fb.Event("E", 500.0f);
  h.Load("g", fb.Build("G"));
  h.sys->Play(HashId("E"), NoEmitter(), nullptr);
  h.Steps(4);
  const float level = h.LastMean(0);
  SND_REQUIRE(level > 0.3f);
  SND_CHECK_EQ(h.sys->SetEffectParam(SND_ID_NONE, 0, 2, 0.25f, 0.0f), SND_OK);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0) / level, 0.25, 1e-5);
  SND_CHECK_NEAR(h.LastMean(1) / level, 0.25, 1e-5);
}

// A9: several commands in one block: the last one recorded wins.
SND_TEST(IndepG2Param, SeveralCommandsInOneBlockTheLastWins) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.2f, 0.0f);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.7f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.7, 1e-5);
  // A fade, then a fade-0 to another value in the same block: the second command wins at once.
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.0f, 1.0f);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.4f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.4, 1e-5);
}

// A10: effect_index counts in the bus's chain; the same kind on two buses moves on one only.
SND_TEST(IndepG2Param, IndexInTheChainAndOneBusOnly) {
  Fx h(G2Config());
  FxBank fb;
  // rows: Sfx 0, Sfx2 1, Master 2
  fb.b.buses = {TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}}, TBus{HashId("Sfx2"), 2u, 0, 0, 0.0f, {}, {}},
                TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}}};
  const std::uint32_t a = fb.Effect(dsp::kEffectEcho, EchoGain(1.0f));
  const std::uint32_t b = fb.Effect(dsp::kEffectEcho, EchoGain(1.0f));
  const std::uint32_t c = fb.Effect(dsp::kEffectEcho, EchoGain(1.0f));
  fb.b.buses[0].effects = {a, b};
  fb.b.buses[1].effects = {c};
  fb.Event("E", 500.0f, 0);
  fb.Event("F", 100.0f, 1);
  h.Load("g", fb.Build("G"));
  h.sys->Play(HashId("E"), NoEmitter(), nullptr);
  h.Steps(4);
  const float e = h.LastMean(0);
  h.sys->SetEffectParam(HashId("Sfx"), 1, 2, 0.5f, 0.0f);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0) / e, 0.5, 1e-5);
  h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.5f, 0.0f);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0) / e, 0.25, 1e-5);
  // Sfx2 still full: F (DC 0.1) plus E at a quarter.
  h.sys->Play(HashId("F"), NoEmitter(), nullptr);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0), 0.25 * e + e * 0.2f, 1e-5);
  h.sys->SetEffectParam(HashId("Sfx2"), 0, 2, 0.0f, 0.0f);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0), 0.25 * e, 1e-5);
}

// Decision: a bank defining the bus unloaded and loaded again starts from the bank's values.
SND_TEST(IndepG2Param, BankLoadedAgainStartsFromTheBankValues) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.5f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.5, 1e-5);
  r.h.sys->UnloadBank(r.bank, SND_UNLOAD_STOP_AND_UNLOAD);
  r.h.Steps(8);
  FxBank fb;
  fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, EchoGain(1.0f))};
  fb.Event("E", 500.0f);
  r.h.Load("g2", fb.Build("G"));
  r.h.sys->Play(HashId("E"), NoEmitter(), nullptr);
  r.h.Steps(4);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 1.0, 1e-5);
}

// A12: SetBusVolume variant 0 is the bus volume, bit for bit; an unknown variant is dropped.
// The raw commands enter through LiveTap::replay (applied like a producer's arena).
SND_TEST(IndepG2Param, BusVolumeVariantZeroIsUnchangedAndUnknownVariantsAreDropped) {
  auto run = [](int mode) {
    RawFx h;
    std::vector<float> out;
    if (h.sys == nullptr) return out;
    FxBank fb;
    fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, EchoGain(1.0f))};
    fb.Event("E", 500.0f);
    h.Load("g", fb.Build("G"));
    h.sys->Play(HashId("E"), NoEmitter(), nullptr);
    h.Steps(4);
    const std::size_t f0 = h.all.size();
    snd::cmd::SetBusVolume p{};
    p.bus = HashId("Sfx");
    p.volume = 0.3f;
    p.fadeSeconds = 3 * kBlockSec;
    if (mode == 0) h.sys->SetBusVolume(HashId("Sfx"), 0.3f, 3 * kBlockSec);
    if (mode == 1) h.Queue(snd::cmd::CommandType::SetBusVolume, p);
    if (mode == 2) {
      p.reserved = 0x37u;  // unknown variant
      h.Queue(snd::cmd::CommandType::SetBusVolume, p);
    }
    if (mode == 3) {
      p.reserved = 0xFFu;  // unknown variant with odd values
      p.volume = std::nanf("");
      p.fadeSeconds = -1.0f;
      h.Queue(snd::cmd::CommandType::SetBusVolume, p);
    }
    if (mode == 5) {  // variant 0 with the upper bits set: still the bus volume? (spec: variant = low 8 bits)
      p.reserved = 0xABCD00u;
      h.Queue(snd::cmd::CommandType::SetBusVolume, p);
    }
    h.Steps(8);
    out.assign(h.all.begin() + static_cast<std::ptrdiff_t>(f0), h.all.end());
    return out;
  };
  const std::vector<float> api = run(0), raw0 = run(1), none = run(4), unk = run(2), unk2 = run(3), hi = run(5);
  SND_REQUIRE(!api.empty());
  SND_CHECK(api != none);  // the bus volume moved
  SND_CHECK(api == raw0);
  SND_CHECK(unk == none);
  SND_CHECK(unk2 == none);
  SND_CHECK(hi == api);
}

// D-130 recorded as SetBusVolume variant 1: the public call and nothing else changes the effect.
SND_TEST(IndepG2Param, EffectParamApiDoesNotTouchTheBusVolume) {
  GainRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.5f, 0.0f);
  r.h.Steps(3);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.5, 1e-5);
  r.h.sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.0f);  // volume and effect multiply
  r.h.Steps(4);
  SND_CHECK_NEAR(r.h.LastMean(0) / r.level, 0.25, 1e-4);
}

// A13: same script -> same bits; RT workers 0 and 2 -> same bits.
SND_TEST(IndepG2Param, DeterministicSerialAndParallel) {
  auto run = [](std::uint32_t workers) {
    snd_config cfg = G2Config();
    cfg.jobs.realtime_workers = workers;
    cfg.jobs.voices_per_chunk = 1;
    Fx h(cfg);
    FxBank fb;
    fb.b.buses = {TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}}, TBus{HashId("Sfx2"), 2u, 0, 0, 0.0f, {}, {}},
                  TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}}};
    fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, {37.0f, 0.6f, 1.0f, 0.7f, 100.0f}),
                             fb.Effect(dsp::kEffectReverb, {})};
    fb.b.buses[1].effects = {fb.Effect(dsp::kEffectDelay, {20.0f, 0.5f, 0.0f, -3.0f, 4000.0f, 1.0f, 50.0f})};
    fb.Event("E", 1000.0f, 0);
    fb.Event("F", 2000.0f + 3000.0f, 1);
    fb.Event("G", 300.0f, 1);
    h.Load("g", fb.Build("G"));
    for (int i = 0; i < 6; ++i) h.sys->Play(HashId(i % 3 == 0 ? "E" : (i % 3 == 1 ? "F" : "G")), NoEmitter(), nullptr);
    for (int b = 0; b < 40; ++b) {
      if (b == 3) h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 5.0f, 0.05f);
      if (b == 7) h.sys->SetEffectParam(HashId("Sfx"), 1, 4, 0.3f, 0.02f);
      if (b == 9) h.sys->SetEffectParam(HashId("Sfx2"), 0, 0, 3.0f, 0.0f);
      if (b == 11) h.sys->SetEffectParam(HashId("Sfx"), 0, 2, 0.1f, 0.1f);
      h.Step();
    }
    return h.all;
  };
  const std::vector<float> a = run(0), b = run(0), c = run(2);
  SND_REQUIRE(!a.empty());
  SND_CHECK(MaxAbsAll(a) > 0.1f);
  SND_CHECK(a == b);
  SND_CHECK(a == c);
}

// A14: no allocation after initialisation, no RT violation, on the new paths.
SND_TEST(IndepG2Param, NoAllocationAndNoRtViolation) {
  g2::CountingAllocator alloc;
  {
    snd_config cfg = G2Config();
    cfg.jobs.realtime_workers = 2;
    Fx h(cfg, {}, &alloc);
    FxBank fb;
    fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, EchoGain(1.0f)), fb.Effect(dsp::kEffectCompressor, {})};
    fb.b.ducks.push_back(TDuck{0, 0, 0, 0.0f, 10, 200, 0});
    fb.Event("E", 500.0f);
    h.Load("g", fb.Build("G"));
    h.Steps(2);
    const std::uint64_t before = alloc.allocations.load();
    snd::test::rt::Reset();
    for (int i = 0; i < 50; ++i) {
      if (i % 10 == 0) h.sys->Play(HashId("E"), NoEmitter(), nullptr);
      h.sys->SetEffectParam(HashId("Sfx"), static_cast<std::uint32_t>(i % 2), static_cast<std::uint32_t>(i % 5),
                            0.1f * float(i % 7), i % 3 == 0 ? 0.0f : 0.02f);
      h.sys->SetEffectParam(HashId("Nope"), 0, 0, 1.0f, 0.0f);
      h.sys->Update();
      tAudioThread = true;
      {
        SND_RT_SCOPE();
        h.sys->RenderBlock(h.out.data());
      }
    }
    SND_CHECK_EQ(alloc.allocations.load(), before);
    SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  }
}

// ---------------------------------------------------------------------------------------
// D-131 compressor sidechain. Rows are ordered by `keyFirst`: Key before / after Sfx.
// Sfx (T) has a compressor (threshold -20 dB, ratio 4, knee 0, fast attack / release);
// DUCK kind 0 Key -> Sfx with amount 0 dB (the rule ducking itself does nothing).
// T plays DC 0.1 (-23 dB at the centre pan: below the threshold), the key DC 0.9 (-3.9 dB).
namespace {
struct SideRig {
  SideRig(bool keyFirst, float useSidechain, std::uint32_t keyChannels = 0, bool selfKey = false)
      : h(G2Config()) {
    FxBank fb;
    const std::uint32_t t = keyFirst ? 1u : 0u, k = keyFirst ? 0u : 1u;
    fb.b.buses.clear();
    fb.b.buses.resize(3);
    fb.b.buses[t] = TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}};
    fb.b.buses[k] = TBus{HashId("Key"), 2u, keyChannels, 0, 0.0f, {}, {}};
    fb.b.buses[2] = TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}};
    fb.b.buses[t].effects = {
        fb.Effect(dsp::kEffectCompressor, {-20.0f, 4.0f, 0.05f, 1.0f, 0.0f, 0.0f, 0.0f, useSidechain})};
    fb.b.ducks.push_back(TDuck{0, selfKey ? t : k, t, 0.0f, 10, 200, 0});
    fb.Event("T", 100.0f, t);
    fb.Event("K", 900.0f, k);
    fb.Event("K2", 2000.0f + 2.0f * 256.0f, k);  // two blocks of DC 1.0
    h.Load("g", fb.Build("G"));
    tBus = t;
  }
  Fx h;
  std::uint32_t tBus = 0;
};
// T's share of the master = master - the key alone (no effect on the key path, linear mix).
constexpr float kKey = 0.9f * kCenter;
}  // namespace

// B1, B10, B5 (same block when the key bus comes first in the graph).
SND_TEST(IndepG2Side, KeyBusDrivesTheGainReductionInTheSameBlockWhenItComesFirst) {
  SideRig r(true, 1.0f);
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  r.h.Steps(4);
  const float alone = r.h.LastMean(0);
  SND_CHECK_NEAR(alone, 0.1f * kCenter, 2e-4);  // below the threshold: unity
  const std::size_t f0 = r.h.Total();
  r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  r.h.Steps(8);
  const std::size_t ks = FirstAbove(r.h, f0, 0.3f);
  SND_REQUIRE(ks != static_cast<std::size_t>(-1));
  // Expected steady gain: key at -3.92 dB, 16.08 dB over the threshold, ratio 4: -12.06 dB.
  const double keyDb = Db(kKey);
  const double expDb = (-20.0 + (keyDb + 20.0) / 4.0) - keyDb;
  const double tShare = r.h.LastMean(0) - kKey;
  std::printf("  T share %.6f (%.2f dB, expected %.2f dB)\n", tShare, Db(tShare / alone), expDb);
  SND_CHECK_NEAR(Db(tShare / alone), expDb, 0.5);
  // Same block: T is already reduced 64 frames after the key starts (attack 0.05 ms).
  const double early = r.h.Mean(ks + 64, ks + 128) - kKey;
  SND_CHECK_MSG(Db(early / alone) < expDb + 1.0, "64 frames after the key: %.2f dB", Db(early / alone));
}

// B5: key after the target in graph order: the previous block's key output.
SND_TEST(IndepG2Side, KeyBusAfterTheTargetActsOneBlockLater) {
  SideRig r(false, 1.0f);
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  r.h.Steps(4);
  const float alone = r.h.LastMean(0);
  const std::size_t f0 = r.h.Total();
  r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  r.h.Steps(6);
  const std::size_t ks = FirstAbove(r.h, f0, 0.3f);
  SND_REQUIRE(ks != static_cast<std::size_t>(-1));
  const std::size_t blockEnd = (ks / kB + 1) * kB;
  // Rest of the key's first block: T still at unity.
  if (blockEnd - ks > 8) SND_CHECK_NEAR((r.h.Mean(ks, blockEnd) - kKey) / alone, 1.0, 1e-3);
  // Next block: reduced.
  SND_CHECK(Db((r.h.Mean(blockEnd + 64, blockEnd + 256) - kKey) / alone) < -10.0);
}

// B2: UseSidechain 0: the compressor detects T itself (below the threshold: unity).
SND_TEST(IndepG2Side, UseSidechainZeroDetectsTheBusItself) {
  SideRig r(true, 0.0f);
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  r.h.Steps(4);
  const float alone = r.h.LastMean(0);
  r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  r.h.Steps(6);
  SND_CHECK_NEAR((r.h.LastMean(0) - kKey) / alone, 1.0, 1e-3);
}

// B3 / B4: a silent key (never played, or paused) does not reduce T; after the key stops, T recovers.
SND_TEST(IndepG2Side, SilentOrPausedKeyDoesNotReduce) {
  SideRig r(true, 1.0f);
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  r.h.Steps(6);
  const float alone = r.h.LastMean(0);
  SND_CHECK_NEAR(alone, 0.1f * kCenter, 2e-4);
  const snd_playback_handle k = r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  r.h.Steps(6);
  SND_CHECK(Db((r.h.LastMean(0) - kKey) / alone) < -10.0);
  r.h.sys->Pause(k, 0.0f);
  r.h.Steps(10);  // release 1 ms
  SND_CHECK_NEAR(r.h.LastMean(0) / alone, 1.0, 1e-3);
  r.h.sys->ResumePlayback(k, 0.0f);
  r.h.Steps(6);
  SND_CHECK(Db((r.h.LastMean(0) - kKey) / alone) < -10.0);
  r.h.sys->Stop(k, 0.0f, nullptr);
  r.h.Steps(10);
  SND_CHECK_NEAR(r.h.LastMean(0) / alone, 1.0, 1e-3);
}

// Decision: the sidechain is the key bus's output after its fader (a muted key does not reduce;
// a key at half volume reduces as a key 6 dB lower).
SND_TEST(IndepG2Side, SidechainIsTakenAfterTheKeyBusFader) {
  SideRig r(true, 1.0f);
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  r.h.Steps(4);
  const float alone = r.h.LastMean(0);
  r.h.sys->SetBusVolume(HashId("Key"), 0.0f, 0.0f);
  r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  r.h.Steps(10);
  SND_CHECK_NEAR(Db(r.h.LastMean(0) / alone), 0.0, 0.01);
  r.h.sys->SetBusVolume(HashId("Key"), 0.5f, 0.0f);
  r.h.Steps(10);
  const double key = Db(0.5 * kKey);
  const double exp = (-20.0 + (key + 20.0) / 4.0) - key;
  SND_CHECK_NEAR(Db((r.h.LastMean(0) - 0.5 * kKey) / alone), exp, 0.3);
}

// B8: a mono key bus reduces as much as a stereo one (peak detector).
SND_TEST(IndepG2Side, MonoKeyBusSameReduction) {
  double red[2] = {0, 0};
  for (int m = 0; m < 2; ++m) {
    SideRig r(true, 1.0f, m == 0 ? 1u : 2u);
    SND_REQUIRE(r.h.sys != nullptr);
    r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
    r.h.Steps(4);
    const float alone = r.h.LastMean(0);
    r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
    r.h.Steps(8);
    // The key alone at the master: whatever the mono bus's pan gives; measure it from a key-only rig.
    SideRig k(true, 1.0f, m == 0 ? 1u : 2u);
    k.h.sys->Play(HashId("K"), NoEmitter(), nullptr);
    k.h.Steps(12);
    const float keyAlone = k.h.LastMean(0);
    red[m] = Db((r.h.LastMean(0) - keyAlone) / alone);
    std::printf("  key channels %d: key at master %.4f, T reduced %.2f dB\n", m == 0 ? 1 : 2, keyAlone, red[m]);
  }
  // Mono bus: the key's peak is 0.9 (no pan inside the key bus), 3 dB above the stereo bus's 0.636.
  const double mono = (-20.0 + (Db(0.9) + 20.0) / 4.0) - Db(0.9);
  const double stereo = (-20.0 + (Db(kKey) + 20.0) / 4.0) - Db(kKey);
  SND_CHECK_NEAR(red[0], mono, 0.6);
  SND_CHECK_NEAR(red[1], stereo, 0.6);
}

// Decision: several DUCK kind 0 rows on one target: the first row (bank order) is the sidechain.
SND_TEST(IndepG2Side, OnlyTheFirstDuckRowFeedsTheSidechain) {
  Fx h(G2Config());
  FxBank fb;
  fb.b.buses = {TBus{HashId("K1"), 3u, 0, 0, 0.0f, {}, {}}, TBus{HashId("K2"), 3u, 0, 0, 0.0f, {}, {}},
                TBus{HashId("Sfx"), 3u, 0, 0, 0.0f, {}, {}}, TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}}};
  fb.b.buses[2].effects = {fb.Effect(dsp::kEffectCompressor, {-20.0f, 4.0f, 0.05f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f})};
  fb.b.ducks.push_back(TDuck{0, 0, 2, 0.0f, 10, 200, 0});
  fb.b.ducks.push_back(TDuck{0, 1, 2, 0.0f, 10, 200, 0});
  fb.Event("T", 100.0f, 2);
  fb.Event("A", 900.0f, 0);
  fb.Event("B", 900.0f, 1);
  h.Load("g", fb.Build("G"));
  h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  h.Steps(4);
  const float alone = h.LastMean(0);
  h.sys->Play(HashId("B"), NoEmitter(), nullptr);
  h.Steps(8);
  const double bySecond = Db((h.LastMean(0) - kKey) / alone);
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(8);
  const double byBoth = Db((h.LastMean(0) - 2 * kKey) / alone);
  const double one = (-20.0 + (Db(kKey) + 20.0) / 4.0) - Db(kKey);
  SND_CHECK_NEAR(bySecond, 0.0, 0.01);  // the second row's key alone: nothing
  SND_CHECK_NEAR(byBoth, one, 0.3);     // the first row's key: as one key
  h.sys->Stop(a, 0.0f, nullptr);
  h.Steps(10);
  SND_CHECK_NEAR(Db((h.LastMean(0) - kKey) / alone), 0.0, 0.01);
}

// B7: the target as its own key: no runaway, finite, bounded.
SND_TEST(IndepG2Side, SelfKeyedStaysBounded) {
  SideRig r(true, 1.0f, 0, true);
  SND_REQUIRE(r.h.sys != nullptr);
  r.h.sys->Play(HashId("K"), NoEmitter(), nullptr);  // the loud key on its own bus (not compressed)
  r.h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  for (int i = 0; i < 40; ++i) r.h.Step();
  SND_CHECK(AllFinite(r.h.all));
  SND_CHECK(MaxAbsAll(r.h.all) < 1.0f);
  // T alone (-23 dB) is below the threshold, keyed by itself: unity.
  SND_CHECK_NEAR(r.h.LastMean(0) - kKey, 0.1f * kCenter, 2e-4);
}

// ---------------------------------------------------------------------------------------
// F1 (2.4): a bus whose effects the load LOD may bypass keeps its tail.
SND_TEST(IndepG2Lod, BypassWaitsForTheTail) {
  snd_config cfg = G2Config();
  cfg.tuning.lod_up_blocks = 0x7FFFFFFFu;  // stay at the level the test sets
  Fx h(cfg);
  FxBank fb;
  fb.b.buses[0].flags = bf::kBusLodBypassable;
  // Echo 20 ms (3.75 blocks), feedback 0.5, wet only.
  fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, {20.0f, 0.5f, 0.0f, 1.0f, 20.0f})};
  fb.Event("P", 2000.0f + 128.0f);  // 128 frames of DC 1.0
  h.Load("g", fb.Build("G"));
  const std::size_t in0 = h.Total();  // the voice starts with the block that applies the Play
  h.sys->Play(HashId("P"), NoEmitter(), nullptr);
  h.Steps(2);
  // The burst went in (wet only: nothing on the output yet); the first echo comes 960 frames
  // after the input start, while the LOD asks for the bypass.
  SND_CHECK(h.MaxAbs(in0, h.Total()) < 1e-6f);
  h.sys->SetLodLevelForTesting(3);
  h.Steps(16);
  snd_stats st{};
  h.sys->GetStats(&st);
  SND_CHECK_EQ(st.lod_level, 3u);
  // Echoes 1..4 after the LOD change (1, 0.5, 0.25, 0.125 of the burst, at the centre pan).
  for (int k = 1; k <= 4; ++k) {
    const std::size_t at = in0 + std::size_t(960 * k) + 32;
    const float exp = kCenter * std::pow(0.5f, float(k - 1));
    SND_CHECK_MSG(std::fabs(h.At(at, 0) - exp) < 0.02f, "echo %d at %llu: %.4f, expected %.4f", k, static_cast<unsigned long long>(at), h.At(at, 0),
                  exp);
  }
}
