// Independent tests: voice effects (D-132 .. D-134). See indep_g2_fixture.hpp.
//
// The usual signature: a voice effect "echo as a pure delay of 10 ms" (wet only, no
// feedback). A wet voice is heard 480 frames after it starts; a dry one at once.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "harness.hpp"
#include "indep_g2_fixture.hpp"
#include "snd/test/rt_guard.hpp"

namespace {

using namespace g2;
using snd::HashId;

constexpr std::uint32_t kB = 256;
constexpr std::size_t kDelay = 480;  // 10 ms at 48 kHz
const std::vector<float> kDelay10 = EchoDelay(10.0f);
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

snd_config VfxConfig(std::uint32_t slots = 4, std::uint32_t chain = 2) {
  snd_config c = G2Config();
  c.max_voice_effects = slots;
  c.voice_effect_chain = chain;
  return c;
}

snd_result SetDelay(Fx& h, const char* event, std::uint32_t slot = 0, float ms = 10.0f) {
  const std::vector<float> p = EchoDelay(ms);
  return h.sys->SetEventVoiceEffect(HashId(event), slot, dsp::kEffectEcho, p.data(), static_cast<std::uint32_t>(p.size()));
}

std::uint32_t CountWarnings(const Fx& h, std::uint64_t object = 0) {
  std::uint32_t n = 0;
  for (const Err& e : h.errors) {
    if (e.code == SND_ERR_LIMIT_REACHED && e.detail == SND_END_DETAIL_VOICE_EFFECT_SLOTS &&
        (object == 0 || e.object == object)) {
      ++n;
    }
  }
  return n;
}

// Bank: Sfx -> Master; events by name with DC levels (IndepKind frequency codes).
std::vector<std::uint8_t> Events(std::initializer_list<std::pair<const char*, float>> ev, std::uint32_t channels = 1) {
  FxBank fb;
  for (const auto& e : ev) fb.Event(e.first, e.second, 0, 128, channels);
  return fb.Build("V");
}

}  // namespace

// C1, C13: only voices that start after the setting is applied; removing the stage
// (SND_ID_NONE) leaves running voices wet and makes new ones dry.
SND_TEST(IndepG2Vfx, OnlyVoicesStartedAfterTheSettingAreAffected) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"E", 500.0f}}));
  const float L = 0.5f * kCenter;
  std::size_t f0 = h.Total();
  h.sys->Play(HashId("E"), NoEmitter(), nullptr);  // P1: dry
  h.Steps(3);
  SND_CHECK_NEAR(h.At(f0, 0), L, 1e-6);
  SND_CHECK_EQ(SetDelay(h, "E"), SND_OK);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0), L, 1e-6);  // P1 unchanged
  f0 = h.Total();
  h.sys->Play(HashId("E"), NoEmitter(), nullptr);  // P2: wet
  h.Steps(4);
  SND_CHECK_NEAR(h.Mean(f0, f0 + kDelay), L, 1e-6);
  SND_CHECK_NEAR(h.Mean(f0 + kDelay, f0 + 4 * kB), 2 * L, 1e-5);
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("E"), 0, SND_ID_NONE, nullptr, 0), SND_OK);
  h.Steps(2);
  SND_CHECK_NEAR(h.LastMean(0), 2 * L, 1e-5);  // P2 still wet (and still sounding)
  f0 = h.Total();
  h.sys->Play(HashId("E"), NoEmitter(), nullptr);  // P3: dry again
  h.Steps(2);
  SND_CHECK_NEAR(h.At(f0 + 1, 0), 3 * L, 1e-5);
}

// C2: voices without voice effects are bit-identical whether the pool exists or not and
// whether other events have voice effects.
SND_TEST(IndepG2Vfx, VoicesWithoutEffectsAreBitIdentical) {
  auto run = [](snd_config cfg, bool configureOther, bool playOther) {
    Fx h(cfg);
    std::vector<float> out;
    if (h.sys == nullptr) return out;
    h.Load("v", Events({{"F", 1000.0f}, {"G", 2000.0f + 700.0f}, {"W", 300.0f}}));
    if (configureOther) SetDelay(h, "W");
    for (int b = 0; b < 30; ++b) {
      if (b % 7 == 0) h.sys->Play(HashId(b % 2 == 0 ? "F" : "G"), NoEmitter(), nullptr);
      if (playOther && b == 3) h.sys->Play(HashId("W"), NoEmitter(), nullptr);
      if (b == 12) h.sys->SetBusVolume(HashId("Sfx"), 0.5f, 0.02f);
      h.Step();
    }
    return h.all;
  };
  snd_config none = VfxConfig(0, 0);
  const std::vector<float> base = run(none, false, false);
  SND_REQUIRE(!base.empty());
  SND_CHECK(run(VfxConfig(), false, false) == base);
  SND_CHECK(run(VfxConfig(), true, false) == base);
  snd_config defaults = G2Config();  // snd_config_init's pool sizes
  SND_CHECK(run(defaults, true, false) == base);
}

// C3: exactly max_voice_effects wet voices; one more plays dry (not rejected) with the
// warning LIMIT_REACHED / VOICE_EFFECT_SLOTS on that playback.
SND_TEST(IndepG2Vfx, OneVoiceOverThePoolPlaysDryWithAWarning) {
  for (int extra = 0; extra < 2; ++extra) {
    Fx h(VfxConfig(2));
    SND_REQUIRE(h.sys != nullptr);
    h.Load("v", Events({{"A", 100.0f}, {"B", 200.0f}, {"C", 400.0f}}));
    SetDelay(h, "A");
    SetDelay(h, "B");
    SetDelay(h, "C");
    h.Steps(1);
    h.errors.clear();
    const std::size_t f0 = h.Total();
    snd_playback_handle p[3];
    p[0] = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
    p[1] = h.sys->Play(HashId("B"), NoEmitter(), nullptr);
    if (extra != 0) p[2] = h.sys->Play(HashId("C"), NoEmitter(), nullptr);
    h.Steps(6);
    const float sum = (extra != 0 ? 0.7f : 0.3f) * kCenter;
    SND_CHECK_NEAR(h.LastMean(0), sum, 1e-5);  // every voice sounds
    const float early = h.Mean(f0 + 8, f0 + kDelay);
    if (extra == 0) {
      SND_CHECK_NEAR(early, 0.0f, 1e-6);  // both wet
      SND_CHECK_EQ(CountWarnings(h), 0u);
    } else {
      // One dry voice: its level is heard at once; which one is not specified.
      const bool oneDry = std::fabs(early - 0.1f * kCenter) < 1e-5f || std::fabs(early - 0.2f * kCenter) < 1e-5f ||
                          std::fabs(early - 0.4f * kCenter) < 1e-5f;
      SND_CHECK_MSG(oneDry, "early level %.6f is not one voice", early);
      SND_CHECK_EQ(CountWarnings(h), 1u);
      std::uint32_t onHandles = 0;
      for (const auto& x : p) onHandles += CountWarnings(h, x.value);
      SND_CHECK_EQ(onHandles, 1u);
      for (const auto& x : p) SND_CHECK_EQ(h.CountEnds(x.value), 0u);
    }
    const auto st = h.sys->GetVoiceFxStats();
    SND_CHECK_EQ(st.slots, 2u);
    SND_CHECK_EQ(st.slotsInUse, 2u);
    SND_CHECK_EQ(st.dry, std::uint64_t(extra));
  }
}

// C4: a virtualised voice gives its slot back; a voice that then becomes real gets it.
SND_TEST(IndepG2Vfx, VirtualisedVoiceReturnsItsSlot) {
  snd_config cfg = VfxConfig(1);
  cfg.max_real_voices = 2;
  Fx h(cfg);
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  fb.Event("A", 100.0f, 0, 100);
  fb.Event("H", 200.0f, 0, 200);
  fb.Event("B", 400.0f, 0, 150);
  h.Load("v", fb.Build("V"));
  SetDelay(h, "A");
  SetDelay(h, "B");
  h.Steps(1);
  h.errors.clear();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);
  const snd_playback_handle h1 = h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  h.Steps(8);  // A virtual (short fade done)
  SND_CHECK_NEAR(h.LastMean(0), 0.4f * kCenter, 1e-5);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 0u);
  h.sys->Stop(h1, 0.0f, nullptr);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("B"), NoEmitter(), nullptr);  // priority 150 > A's 100: B gets the real voice
  h.Steps(6);
  SND_CHECK_EQ(CountWarnings(h), 0u);  // B wet: the slot came back
  SND_CHECK_NEAR(h.Mean(f0 + 300, f0 + kDelay), 0.2f * kCenter, 1e-5);  // the stopped H's short fade is over
  SND_CHECK_NEAR(h.LastMean(0), 0.6f * kCenter, 1e-5);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);
}

// C5: a slot used again starts from a fresh state (no echo of the previous voice).
SND_TEST(IndepG2Vfx, ReusedSlotDoesNotLeakThePreviousVoice) {
  Fx h(VfxConfig(1));
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 2000.0f + 1024.0f}}));  // 1024 frames of DC 1.0
  SetDelay(h, "A");
  h.Steps(1);
  h.errors.clear();
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(5);  // A ends at f0 + 1024; its delay line still holds 480 frames of 1.0
  const std::size_t f1 = h.Total();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  SND_CHECK_NEAR(h.Mean(f0 + kDelay, f0 + 1024), kCenter, 1e-5);
  SND_CHECK_EQ(CountWarnings(h), 0u);  // the second voice got the slot
  SND_CHECK_MSG(h.MaxAbs(f1, f1 + kDelay) < 1e-6f, "second voice's first 480 frames: max %.6f (leak)",
                h.MaxAbs(f1, f1 + kDelay));
  SND_CHECK_NEAR(h.Mean(f1 + kDelay, f1 + 1024), kCenter, 1e-5);
}

// C6 / D-134: no tail past the voice's end.
SND_TEST(IndepG2Vfx, NoTailPastTheVoicesEnd) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 2000.0f + 1024.0f}}));
  SetDelay(h, "A");
  h.Steps(1);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(10);
  SND_CHECK_NEAR(h.Mean(f0 + kDelay, f0 + 1024), kCenter, 1e-5);
  SND_CHECK_MSG(h.MaxAbs(f0 + 1024, h.Total()) < 1e-6f, "after the voice's end: max %.6f",
                h.MaxAbs(f0 + 1024, h.Total()));
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 0u);
}

// C6: the stop fade is applied after the voice effect: the wet output fades from the stop.
SND_TEST(IndepG2Vfx, StopFadeAppliesAfterTheEffect) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 999.0f}}));  // DC 0.999 endless
  SetDelay(h, "A");
  h.Steps(1);
  const snd_playback_handle p = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(6);
  const float L = h.LastMean(0);
  SND_REQUIRE(L > 0.6f);
  const std::size_t s = h.Total();
  h.sys->Stop(p, 0.01f, nullptr);  // 480 frames
  h.Steps(6);
  // From the stop on, the output falls: half way through the fade it is about half.
  SND_CHECK_MSG(std::fabs(h.At(s + 240, 0) / L - 0.5f) < 0.1f, "at the fade's middle: %.3f of the level",
                h.At(s + 240, 0) / L);
  SND_CHECK(h.At(s + 400, 0) < 0.25f * L);
  // The stop fade ends with the block that reaches it (core ramps gains per block): silent after
  // that block, and not the delay line's 480 frames of tail.
  SND_CHECK(h.MaxAbs(s + 2 * kB, h.Total()) < 1e-6f);
}

// C7: the voice effect runs before the pan: a ping-pong delay on a mono voice stays mono.
SND_TEST(IndepG2Vfx, EffectRunsBeforeThePan) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 2000.0f + 64.0f}}));  // a 64-frame burst
  const float p[] = {5.0f, 0.6f, -96.0f, 0.0f, 20000.0f, 1.0f, 10.0f};
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectDelay, p, 7), SND_OK);
  h.Steps(1);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(2);
  SND_CHECK(h.MaxAbs(f0 + 240, f0 + 64 + 240) > 0.3f);  // first repeat after 5 ms (while the voice lasts)
  double diff = 0;
  for (std::size_t f = f0; f < h.Total(); ++f) diff = std::fmax(diff, std::fabs(h.At(f, 0) - h.At(f, 1)));
  SND_CHECK_MSG(diff == 0.0, "left and right differ by %g: the effect ran on the panned stereo signal", diff);
}

// C8: stages run in slot order: slot 0 gain 0.2 then slot 1 distortion = dist(0.2 x).
SND_TEST(IndepG2Vfx, StagesRunInSlotOrder) {
  // Offline reference with the dsp module.
  auto offline = [](bool gainFirst) {
    const float g[] = {10.0f, 0.0f, 0.2f, 0.0f, 10.0f};
    const float d[] = {1.0f, 1.0f, 0.0f, 20000.0f};
    snd::EffectDesc dg{dsp::kEffectEcho, 1, 256, 5, g, nullptr, 0, 48000, 0};
    snd::EffectDesc dd{dsp::kEffectDistortion, 1, 256, 4, d, nullptr, 0, 48000, 0};
    const std::uint32_t bg = dsp::EffectStateBytes(dg, 48000), bd = dsp::EffectStateBytes(dd, 48000);
    void* sg = std::aligned_alloc(64, bg);
    void* sd = std::aligned_alloc(64, bd);
    dsp::EffectInit(sg, bg, dg, 48000);
    dsp::EffectInit(sd, bd, dd, 48000);
    std::vector<float> x(256, 1.0f);
    float* ch[1] = {x.data()};
    for (int b = 0; b < 8; ++b) {
      std::fill(x.begin(), x.end(), 1.0f);
      if (gainFirst) {
        dsp::EffectProcess(sg, ch, ch, 1, 256, nullptr);
        dsp::EffectProcess(sd, ch, ch, 1, 256, nullptr);
      } else {
        dsp::EffectProcess(sd, ch, ch, 1, 256, nullptr);
        dsp::EffectProcess(sg, ch, ch, 1, 256, nullptr);
      }
    }
    const float v = x[255];
    std::free(sg);
    std::free(sd);
    return v;
  };
  const float ref01 = offline(true), ref10 = offline(false);
  SND_REQUIRE(std::fabs(ref01 - ref10) > 0.02f);
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 2000.0f + 100000.0f}}));  // DC 1.0
  const float g[] = {10.0f, 0.0f, 0.2f, 0.0f, 10.0f};
  const float d[] = {1.0f, 1.0f, 0.0f, 20000.0f};
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, g, 5), SND_OK);
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 1, dsp::kEffectDistortion, d, 4), SND_OK);
  h.Steps(1);
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(8);
  const float got = h.LastMean(0) / kCenter;
  std::printf("  slot order: core %.6f, offline gain->dist %.6f, dist->gain %.6f\n", got, ref01, ref10);
  SND_CHECK_NEAR(got, ref01, 1e-4);
}

// C9: per-playback values from the next block, interpolated in the block, kept for voices that
// become real later; argument checks.
SND_TEST(IndepG2Vfx, PlaybackValueAppliesFromTheNextBlockWithoutAStep) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 500.0f}}));
  SetDelay(h, "A");
  h.Steps(1);
  const snd_playback_handle p = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(6);
  const float L = h.LastMean(0);
  SND_REQUIRE(L > 0.3f);
  const std::size_t f0 = h.Total();
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 3, 0.5f), SND_OK);  // WetMix
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(0) / L, 0.5, 1e-5);
  double worst = 0;
  for (std::size_t f = f0; f + 1 < h.Total(); ++f) worst = std::fmax(worst, std::fabs(h.At(f + 1, 0) - h.At(f, 0)));
  SND_CHECK_MSG(worst < 0.5 * L / kB * 1.05 + 1e-6, "largest step %.6f (one-block ramp: %.6f)", worst, 0.5 * L / kB);
  // Clamped like any standard-effect value (WetMix 0..1).
  h.sys->SetPlaybackEffectParam(p, 0, 3, 9.0f);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(0) / L, 1.0, 1e-5);
  // Argument checks.
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(snd_playback_handle{0}, 0, 3, 0.5f), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 2, 3, 0.5f), SND_ERR_INVALID_ARGUMENT);  // chain = 2
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 16, 0.5f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 3, std::nanf("")), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 3, -INFINITY), SND_ERR_INVALID_ARGUMENT);
  h.sys->Stop(p, 0.0f, nullptr);
  h.Steps(4);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 3, 0.5f), SND_ERR_INVALID_HANDLE);  // stale
}

SND_TEST(IndepG2Vfx, PlaybackValueReachesAVoiceThatBecomesRealLater) {
  snd_config cfg = VfxConfig();
  cfg.max_real_voices = 1;
  Fx h(cfg);
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  fb.Event("H", 200.0f, 0, 200);
  fb.Event("A", 500.0f, 0, 100);
  h.Load("v", fb.Build("V"));
  SetDelay(h, "A");
  h.Steps(1);
  const snd_playback_handle hp = h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  h.Steps(2);
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);  // virtual
  h.Steps(2);
  SND_CHECK_NEAR(h.LastMean(0), 0.2f * kCenter, 1e-6);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(a, 0, 3, 0.25f), SND_OK);
  h.Steps(2);
  h.sys->Stop(hp, 0.0f, nullptr);
  h.Steps(10);
  SND_CHECK_NEAR(h.LastMean(0), 0.25f * 0.5f * kCenter, 1e-5);
}

// C10: voice_effect_chain 0 / 4 / 5; voice_effect_bytes / settings ranges; max_voice_effects clamp.
SND_TEST(IndepG2Vfx, ChainLengthLimits) {
  {
    Fx h(VfxConfig(4, 0));
    SND_REQUIRE(h.sys != nullptr);
    h.Load("v", Events({{"A", 500.0f}}));
    SND_CHECK_EQ(SetDelay(h, "A", 0), SND_ERR_INVALID_ARGUMENT);
  }
  {
    Fx h(VfxConfig(4, 4));
    SND_REQUIRE(h.sys != nullptr);
    h.Load("v", Events({{"A", 500.0f}}));
    SND_CHECK_EQ(SetDelay(h, "A", 3), SND_OK);
    SND_CHECK_EQ(SetDelay(h, "A", 4), SND_ERR_INVALID_ARGUMENT);
  }
  {
    Fx h(VfxConfig(4, 5));
    SND_CHECK_EQ(h.created, SND_ERR_INVALID_ARGUMENT);
    SND_CHECK(h.sys == nullptr);
  }
}

SND_TEST(IndepG2Vfx, PoolConfigRanges) {
  {
    snd_config c = VfxConfig();
    c.voice_effect_bytes = (64u << 20) + 64u;  // "at most 64 MiB"
    Fx h(c);
    SND_CHECK_EQ(h.created, SND_ERR_INVALID_ARGUMENT);
  }
  {
    snd_config c = VfxConfig();
    c.voice_effect_settings = 0;  // 1..65535
    Fx h(c);
    SND_CHECK_EQ(h.created, SND_ERR_INVALID_ARGUMENT);
  }
  {
    snd_config c = VfxConfig();
    c.voice_effect_settings = 65536;
    Fx h(c);
    SND_CHECK_EQ(h.created, SND_ERR_INVALID_ARGUMENT);
  }
  {
    snd_config c = VfxConfig(100000);  // clamped to max_real_voices + fade_reserve_voices (8 + 4)
    Fx h(c);
    SND_REQUIRE(h.sys != nullptr);
    SND_CHECK_EQ(h.sys->GetVoiceFxStats().slots, 12u);
  }
}

// C11: a chain whose state does not fit voice_effect_bytes plays dry with the warning.
SND_TEST(IndepG2Vfx, StateTooLargeForTheSlotPlaysDry) {
  snd_config c = VfxConfig();
  c.voice_effect_bytes = 64;
  Fx h(c);
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 500.0f}}));
  SND_CHECK_EQ(SetDelay(h, "A"), SND_OK);
  h.Steps(1);
  h.errors.clear();
  const std::size_t f0 = h.Total();
  const snd_playback_handle p = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  SND_CHECK_NEAR(h.At(f0 + 1, 0), 0.5f * kCenter, 1e-6);  // dry, at once
  SND_CHECK_EQ(CountWarnings(h, p.value), 1u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().dryTooLarge, 1u);
  SND_CHECK_EQ(h.CountEnds(p.value), 0u);
}

// C12: the Event configuration table holds voice_effect_settings entries.
SND_TEST(IndepG2Vfx, SpecHole_SettingsTableFull) {
  snd_config c = VfxConfig();
  c.voice_effect_settings = 2;
  Fx h(c);
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 100.0f}, {"B", 200.0f}, {"C", 400.0f}}));
  SND_CHECK_EQ(SetDelay(h, "A"), SND_OK);
  SND_CHECK_EQ(SetDelay(h, "B"), SND_OK);
  const snd_result third = SetDelay(h, "C");
  h.Steps(1);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.sys->Play(HashId("B"), NoEmitter(), nullptr);
  h.Steps(4);
  SND_CHECK_NEAR(h.Mean(f0 + 8, f0 + kDelay), 0.0f, 1e-6);  // the two that fit are wet
  const std::size_t f1 = h.Total();
  h.sys->Play(HashId("C"), NoEmitter(), nullptr);
  h.Steps(4);
  const bool cWet = std::fabs(h.Mean(f1 + 8, f1 + kDelay) - 0.3f * kCenter) < 1e-5f;
  std::uint32_t warnings = 0;
  for (const Err& e : h.errors) warnings += e.code != SND_OK ? 1u : 0u;
  std::printf("  [spec hole C12] third setting: record %d, C %s, %u warnings\n", third, cWet ? "wet" : "dry", warnings);
  SND_CHECK(true);
}

// C14: argument checks of snd_set_event_voice_effect.
SND_TEST(IndepG2Vfx, EventVoiceEffectArgumentChecks) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  auto* s = h.sys;
  const float p17[17] = {};
  const float bad[] = {10.0f, std::nanf("")};
  SND_CHECK_EQ(s->SetEventVoiceEffect(SND_ID_NONE, 0, dsp::kEffectEcho, nullptr, 0), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEventVoiceEffect(HashId("A"), 2, dsp::kEffectEcho, nullptr, 0), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, p17, 17), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, bad, 2), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(s->SetEventVoiceEffect(HashId("A"), 0, HashId("no.such.effect"), nullptr, 0), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(s->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, nullptr, 0), SND_OK);  // defaults
  SND_CHECK_EQ(s->SetEventVoiceEffect(HashId("A"), 1, SND_ID_NONE, nullptr, 0), SND_OK);
  h.Steps(2);  // an event that is not loaded: nothing happens
  SND_CHECK(AllFinite(h.all));
}

// C15: a plugin effect as a voice effect; per-playback values reach it as one in-block ramp.
// C5 for plugins: a new voice in a reused slot gets a freshly initialised state.
SND_TEST(IndepG2Vfx, PluginVoiceEffect) {
  Log().Reset();
  const snd_dsp_effect probe = ProbePlugin();
  snd_config c = VfxConfig(1);
  c.plugins.effect_count = 1;
  c.plugins.effects = &probe;
  Fx h(c);
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 500.0f}, {"S", 2000.0f + 512.0f}}));
  const float half = 0.5f;
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 0, kProbeId, &half, 1), SND_OK);
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("S"), 0, kProbeId, &half, 1), SND_OK);
  h.Steps(1);
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(0), 0.25f * kCenter, 1e-6);
  const std::uint32_t n0 = Log().Count();
  SND_REQUIRE(n0 > 0u);
  SND_CHECK_EQ(Log().calls[n0 - 1].channels, 1u);  // mono voice: before the pan
  h.sys->SetPlaybackEffectParam(a, 0, 0, 0.25f);
  h.Steps(3);
  const std::uint32_t n1 = Log().Count();
  std::uint32_t ramps = 0;
  for (std::uint32_t i = n0; i < n1; ++i) {
    const ProbeCall& k = Log().calls[i];
    if (k.start0 == 0.5f && k.end0 == 0.25f) ++ramps;
    SND_CHECK(k.start0 == k.end0 || (k.start0 == 0.5f && k.end0 == 0.25f));
  }
  SND_CHECK_EQ(ramps, 1u);
  SND_CHECK_NEAR(h.LastMean(0), 0.125f * kCenter, 1e-6);
  // Slot reuse: stop A, play S (same single slot); its first call must see a fresh state.
  h.sys->Stop(a, 0.0f, nullptr);
  h.Steps(3);
  const std::uint32_t inits = Log().inits.load();
  const std::uint32_t n2 = Log().Count();
  h.sys->Play(HashId("S"), NoEmitter(), nullptr);
  h.Steps(3);
  SND_CHECK(Log().inits.load() > inits);
  SND_REQUIRE(Log().Count() > n2);
  SND_CHECK_EQ(Log().calls[n2].stateLastIn, 0.0f);
}

// C16: a stereo voice: the effect delays both channels (and gets 2 channels).
SND_TEST(IndepG2Vfx, StereoVoice) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 500.0f}}, 2));
  // Reference: the same stereo voice dry.
  const std::size_t r0 = h.Total();
  const snd_playback_handle d = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(3);
  const float dryL = h.At(r0 + 600, 0), dryR = h.At(r0 + 600, 1);
  h.sys->Stop(d, 0.0f, nullptr);
  h.Steps(3);
  SND_REQUIRE(dryL > 0.1f && dryR > 0.1f);
  SetDelay(h, "A");
  h.Steps(1);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  for (std::uint32_t c = 0; c < 2; ++c) {
    SND_CHECK(h.MaxAbs(f0, f0 + kDelay, c) < 1e-6f);
    SND_CHECK_NEAR(h.At(f0 + 600, c), c == 0 ? dryL : dryR, 1e-5);
  }
}

// C17: a looping voice keeps its effect state across the loop point (a 30 ms delay of a
// 1000-frame loop still outputs the earlier periods).
SND_TEST(IndepG2Vfx, LoopingVoiceKeepsTheEffectStateAcrossTheLoop) {
  snd_config cfg = coretest::SmallConfig();
  cfg.tuning.lod_down_blocks = 0x7FFFFFFFu;
  cfg.max_voice_effects = 4;
  coretest::Harness h(cfg);
  SND_REQUIRE(h.sys != nullptr);
  coretest::TestBank b = coretest::BasicBank();
  std::vector<float> pulse(1000, 0.0f);
  for (int i = 0; i < 50; ++i) pulse[static_cast<std::size_t>(i)] = 1.0f;
  b.assets.push_back(coretest::TAsset{HashId("p"), 1, 48000, pulse, 0, 0});
  coretest::TEvent e = coretest::SimpleEvent("L", 0);
  e.tracks[0].clips[0].loopCount = 0;  // endless loop
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  const std::vector<float> p = EchoDelay(30.0f);  // 1440 frames
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("L"), 0, dsp::kEffectEcho, p.data(), 5), SND_OK);
  h.Step();
  const std::size_t f0 = h.TotalFrames();
  h.sys->Play(HashId("L"), snd_emitter_handle{0}, nullptr);
  h.Steps(24);
  auto energy = [&](std::size_t from, std::size_t n) {
    double e2 = 0;
    for (std::size_t f = from; f < from + n; ++f) e2 += double(h.At(f, 0)) * h.At(f, 0);
    return e2;
  };
  SND_CHECK(energy(f0, 1400) < 1e-6);
  for (int k = 0; k < 3; ++k) {
    const std::size_t at = f0 + 1440 + 1000 * std::size_t(k);
    SND_CHECK_MSG(energy(at, 60) > 50 * 0.4 * 0.5, "pulse %d after the loop: energy %.4f", k, energy(at, 60));
  }
}

// C18: pause and resume of a voice with a voice effect.
SND_TEST(IndepG2Vfx, PauseAndResume) {
  Fx h(VfxConfig());
  SND_REQUIRE(h.sys != nullptr);
  h.Load("v", Events({{"A", 500.0f}}));
  SetDelay(h, "A");
  h.Steps(1);
  const snd_playback_handle p = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(6);
  const float L = h.LastMean(0);
  h.sys->Pause(p, 0.0f);
  h.Steps(4);
  SND_CHECK(std::fabs(h.LastMean(0)) < 1e-6f);
  h.sys->ResumePlayback(p, 0.0f);
  h.Steps(6);
  SND_CHECK_NEAR(h.LastMean(0), L, 1e-5);
  SND_CHECK(AllFinite(h.all));
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);
}

// C19: a stolen voice gives its slot back once its fade is over.
SND_TEST(IndepG2Vfx, StolenVoiceReturnsItsSlot) {
  Fx h(VfxConfig(2));
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  fb.b.groups.push_back(TGroup{HashId("G"), 1, 0});
  TEvent& e = fb.Event("A", 500.0f);
  e.group = 0;
  h.Load("v", fb.Build("V"));
  SetDelay(h, "A");
  h.Steps(1);
  h.errors.clear();
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  h.sys->Play(HashId("A"), NoEmitter(), nullptr);  // steals a (oldest)
  h.Steps(8);
  SND_CHECK_EQ(h.CountEnds(a.value), 1u);  // (its end reason belongs to voice management, group 4)
  SND_CHECK_EQ(CountWarnings(h), 0u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);
  SND_CHECK_NEAR(h.LastMean(0), 0.5f * kCenter, 1e-5);
}

// C20: determinism (two runs, RT workers 0 vs 2) with voice effects, slot overflow, plugin.
SND_TEST(IndepG2Vfx, DeterministicSerialAndParallel) {
  static const snd_dsp_effect probe = ProbePlugin();
  auto run = [](std::uint32_t workers) {
    snd_config c = VfxConfig(5, 3);
    c.jobs.realtime_workers = workers;
    c.jobs.voices_per_chunk = 1;
    c.plugins.effect_count = 1;
    c.plugins.effects = &probe;
    Fx h(c);
    std::vector<float> out;
    if (h.sys == nullptr) return out;
    h.Load("v", Events({{"A", 1000.0f}, {"B", 2000.0f + 3000.0f}, {"C", 250.0f}, {"D", 700.0f}}));
    const float rev[] = {0.0f, -500.0f, -100.0f, 0.0f, 0.4f};
    const float ch[] = {0.5f, 0.5f, 7.0f, 2.0f, 0.4f, 0.3f};
    const float pr = 0.7f;
    h.sys->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectReverb, rev, 5);
    h.sys->SetEventVoiceEffect(HashId("A"), 2, kProbeId, &pr, 1);
    h.sys->SetEventVoiceEffect(HashId("B"), 1, dsp::kEffectChorus, ch, 6);
    SetDelay(h, "C");
    h.sys->SetEventVoiceEffect(HashId("D"), 0, dsp::kEffectPitchShifter, nullptr, 0);
    h.Steps(1);
    snd_playback_handle ps[12] = {};
    for (int b = 0; b < 40; ++b) {
      if (b < 12) ps[b] = h.sys->Play(HashId(b % 4 == 0 ? "A" : b % 4 == 1 ? "B" : b % 4 == 2 ? "C" : "D"), NoEmitter(), nullptr);
      if (b == 14) h.sys->SetPlaybackEffectParam(ps[0], 0, 4, 2.0f);
      if (b == 16) h.sys->SetPlaybackEffectParam(ps[3], 0, 0, 1.5f);
      if (b == 20) h.sys->Stop(ps[2], 0.01f, nullptr);
      h.Step();
    }
    return h.all;
  };
  const std::vector<float> a = run(0), b = run(0), c = run(2);
  SND_REQUIRE(!a.empty());
  SND_CHECK(AllFinite(a));
  SND_CHECK(MaxAbsAll(a) > 0.1f);
  SND_CHECK(a == b);
  SND_CHECK(a == c);
  for (std::size_t i = 0; i < a.size() && i < c.size(); ++i) {
    if (a[i] != c[i]) {
      std::printf("  first difference at frame %zu (block %zu): %.7f vs %.7f\n", i / 2, i / 2 / 256, a[i], c[i]);
      break;
    }
  }
}

// C20: no allocation after initialisation and no RT violation with voice effects.
SND_TEST(IndepG2Vfx, NoAllocationAndNoRtViolation) {
  g2::CountingAllocator alloc;
  {
    snd_config c = VfxConfig(2);
    c.jobs.realtime_workers = 2;
    Fx h(c, {}, &alloc);
    SND_REQUIRE(h.sys != nullptr);
    h.Load("v", Events({{"A", 500.0f}, {"B", 2000.0f + 700.0f}}));
    SetDelay(h, "A");
    h.sys->SetEventVoiceEffect(HashId("B"), 1, dsp::kEffectReverb, nullptr, 0);
    h.Steps(2);
    const std::uint64_t before = alloc.allocations.load();
    snd::test::rt::Reset();
    snd_playback_handle last{};
    for (int i = 0; i < 60; ++i) {
      if (i % 5 == 0) last = h.sys->Play(HashId(i % 10 == 0 ? "A" : "B"), NoEmitter(), nullptr);
      if (i % 7 == 0) h.sys->SetPlaybackEffectParam(last, 0, 3, 0.1f * float(i % 10));
      if (i % 11 == 0) SetDelay(h, "A", 1, 5.0f);
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
