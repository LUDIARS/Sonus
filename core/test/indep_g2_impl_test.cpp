// Independent tests written after reading the implementation (step 3 of the tester's brief):
// the places where the voice-effect pool, the per-playback values, the recording of the
// variant commands and the sidechain buffers could break. See indep_g2_fixture.hpp.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "indep_g2_fixture.hpp"

namespace {

using namespace g2;
using snd::HashId;

constexpr std::uint32_t kB = 256;
constexpr std::size_t kDelay = 480;

snd_config VConfig(std::uint32_t slots = 4, std::uint32_t real = 8) {
  snd_config c = G2Config();
  c.max_voice_effects = slots;
  c.max_real_voices = real;
  return c;
}
snd_result SetDelay(Fx& h, const char* event, float ms) {
  const std::vector<float> p = EchoDelay(ms);
  return h.sys->SetEventVoiceEffect(HashId(event), 0, dsp::kEffectEcho, p.data(), 5);
}

// One real voice: H (priority 200, DC 0.2, no effect) pushes A (priority 100, DC 0.5) to virtual.
struct VirtualRig {
  explicit VirtualRig(snd_config cfg = VConfig(4, 1)) : h(cfg) {
    FxBank fb;
    fb.Event("H", 200.0f, 0, 200);
    fb.Event("A", 500.0f, 0, 100);
    h.Load("v", fb.Build("V"));
  }
  Fx h;
};

}  // namespace

// The header (sonus.h): an Event's voice-effect setting applies to "the voices of `event` that
// start after this is applied". A voice that started before a change keeps what it had; the
// implementation re-reads the Event's current parameters when the voice takes a slot again
// (after being virtual), so a returning voice sounds with the new setting.
SND_TEST(IndepG2Impl, Defect_VoiceBackFromVirtualTakesTheEventSettingChangedAfterItStarted) {
  VirtualRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 10.0f), SND_OK);
  r.h.Steps(1);
  r.h.sys->Play(HashId("A"), NoEmitter(), nullptr);  // wet, 10 ms
  r.h.Steps(4);
  const snd_playback_handle hp = r.h.sys->Play(HashId("H"), NoEmitter(), nullptr);  // A -> virtual
  r.h.Steps(6);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 30.0f), SND_OK);  // for voices that start from now on
  r.h.Steps(2);
  const std::size_t f0 = r.h.Total();
  r.h.sys->Stop(hp, 0.0f, nullptr);  // A back to real
  r.h.Steps(10);
  // A started with the 10 ms delay: heard from about 480 frames after its return (fresh state).
  const float mid = r.h.Mean(f0 + 700, f0 + 1400);
  std::printf("  A after its return, frames 700..1400: %.4f (10 ms setting: %.4f, 30 ms: 0)\n", mid, 0.5f * kCenter);
  SND_CHECK_NEAR(mid, 0.5f * kCenter, 1e-3);
}

// Same, removing the stage while the voice is virtual: the voice started with a 10 ms delay.
SND_TEST(IndepG2Impl, Defect_VoiceBackFromVirtualLosesItsSettingWhenTheEventStageWasRemoved) {
  VirtualRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 10.0f), SND_OK);
  r.h.Steps(1);
  r.h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  r.h.Steps(4);
  const snd_playback_handle hp = r.h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  r.h.Steps(6);
  SND_REQUIRE_EQ(r.h.sys->SetEventVoiceEffect(HashId("A"), 0, SND_ID_NONE, nullptr, 0), SND_OK);
  r.h.Steps(2);
  r.h.errors.clear();
  const std::size_t f0 = r.h.Total();
  r.h.sys->Stop(hp, 0.0f, nullptr);
  r.h.Steps(10);
  const float early = r.h.Mean(f0 + 300, f0 + kDelay);  // H's short fade is over; A delayed: silence
  const float mid = r.h.Mean(f0 + 700, f0 + 1400);
  std::printf("  A after its return: early %.4f, 700..1400 %.4f, warnings %llu\n", early, mid, static_cast<unsigned long long>(r.h.errors.size()));
  SND_CHECK_NEAR(early, 0.0f, 1e-4);
  SND_CHECK_NEAR(mid, 0.5f * kCenter, 1e-3);
}

// snd_set_playback_effect_param is "kept for the playback's life (voices that become real
// later use it too)". A voice that becomes real long after the value was set should start at
// that value; the first block after it takes a slot ramps from the Event's value instead.
SND_TEST(IndepG2Impl, Defect_PlaybackValueRampsFromTheEventValueWhenTheVoiceBecomesReal) {
  Log().Reset();
  const snd_dsp_effect probe = ProbePlugin();
  snd_config c = VConfig(4, 1);
  c.plugins.effect_count = 1;
  c.plugins.effects = &probe;
  VirtualRig r(c);
  SND_REQUIRE(r.h.sys != nullptr);
  const float one = 1.0f;
  SND_REQUIRE_EQ(r.h.sys->SetEventVoiceEffect(HashId("A"), 0, kProbeId, &one, 1), SND_OK);
  r.h.Steps(1);
  const snd_playback_handle hp = r.h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  r.h.Steps(2);
  const snd_playback_handle a = r.h.sys->Play(HashId("A"), NoEmitter(), nullptr);  // virtual from the start
  r.h.Steps(2);
  SND_REQUIRE_EQ(r.h.sys->SetPlaybackEffectParam(a, 0, 0, 0.25f), SND_OK);
  r.h.Steps(6);
  SND_CHECK_EQ(Log().Count(), 0u);  // never real so far
  r.h.sys->Stop(hp, 0.0f, nullptr);
  r.h.Steps(4);
  SND_REQUIRE(Log().Count() > 0u);
  const ProbeCall& first = Log().calls[0];
  std::printf("  first block of the real voice: parameter 0 from %.3f to %.3f\n", first.start0, first.end0);
  SND_CHECK_EQ(first.start0, 0.25f);
  SND_CHECK_EQ(first.end0, 0.25f);
}

// sonus.h: SND_ERR_COMMAND_BUFFER_FULL means the command was dropped at record time.
// snd_set_event_voice_effect records 1 + param_count commands; when only some fit, the call
// returns BUFFER_FULL but the ones that fit are applied (the Event gets a partial setting).
SND_TEST(IndepG2Impl, Defect_EventVoiceEffectRefusedForAFullArenaIsPartlyApplied) {
  snd_config cfg = VConfig();
  cfg.commands.arena_bytes = 2048;
  cfg.commands.reserve_bytes = 512;
  cfg.voice_effect_bytes = 1u << 20;  // a partial setting (MaxDelay left at 1 s) still fits a slot
  // Capacity in 32-byte variant commands (SetSwitch variant 1) of one published arena.
  auto capacity = [&] {
    Fx h(cfg);
    if (h.sys == nullptr) return 0u;
    FxBank fb;
    fb.Event("E", 500.0f);
    h.Load("v", fb.Build("V"));
    std::uint32_t n = 0;
    while (n < 10000 && h.sys->SetEventVoiceEffect(HashId("X"), 1, dsp::kEffectEcho, nullptr, 0) == SND_OK) ++n;
    return n;
  };
  const std::uint32_t n = capacity();
  SND_REQUIRE(n > 8u);
  Fx h(cfg);
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  fb.Event("E", 500.0f);
  h.Load("v", fb.Build("V"));
  for (std::uint32_t i = 0; i + 5u < n; ++i) h.sys->SetEventVoiceEffect(HashId("X"), 1, dsp::kEffectEcho, nullptr, 0);
  const std::vector<float> p = EchoDelay(10.0f);  // 1 + 5 commands, 5 fit
  const snd_result rr = h.sys->SetEventVoiceEffect(HashId("E"), 0, dsp::kEffectEcho, p.data(), 5);
  SND_CHECK_EQ(rr, SND_ERR_COMMAND_BUFFER_FULL);
  h.Steps(2);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("E"), NoEmitter(), nullptr);
  h.Steps(4);
  // Nothing of the refused call may apply: E plays dry (heard at once), no warning either.
  std::printf("  warnings: %llu\n", static_cast<unsigned long long>(h.errors.size()));
  std::printf("  E after the refused call: first frames %.4f (dry %.4f, wet: 0)\n", h.Mean(f0 + 8, f0 + kDelay),
              0.5f * kCenter);
  SND_CHECK_NEAR(h.Mean(f0 + 8, f0 + kDelay), 0.5f * kCenter, 1e-4);
}

// D-131, previous-block path: when the key (mixed after the target) stops, the stored
// previous block is renewed with silence and the target recovers.
SND_TEST(IndepG2Impl, SidechainFromThePreviousBlockRecoversWhenTheKeyStops) {
  Fx h(G2Config());
  FxBank fb;
  fb.b.buses = {TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}}, TBus{HashId("Key"), 2u, 0, 0, 0.0f, {}, {}},
                TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}}};
  fb.b.buses[0].effects = {fb.Effect(dsp::kEffectCompressor, {-20.0f, 4.0f, 0.05f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f})};
  fb.b.ducks.push_back(TDuck{0, 1, 0, 0.0f, 10, 200, 0});
  fb.Event("T", 100.0f, 0);
  fb.Event("K", 900.0f, 1);
  h.Load("v", fb.Build("V"));
  h.sys->Play(HashId("T"), NoEmitter(), nullptr);
  h.Steps(4);
  const float alone = h.LastMean(0);
  const snd_playback_handle k = h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  h.Steps(6);
  SND_CHECK(Db((h.LastMean(0) - 0.9f * kCenter) / alone) < -10.0);
  h.sys->Stop(k, 0.0f, nullptr);
  h.Steps(10);
  SND_CHECK_NEAR(h.LastMean(0) / alone, 1.0, 1e-3);
  // Unload of the bank holding the rows: no crash, no NaN after.
  h.sys->Play(HashId("K"), NoEmitter(), nullptr);
  h.Steps(2);
  SND_CHECK(AllFinite(h.all));
}

// Spec hole: a nested Event (CLIP kind 1): whose voice-effect setting do its voices take?
SND_TEST(IndepG2Impl, SpecHole_NestedEventVoicesTakeWhichSetting) {
  for (int which = 0; which < 2; ++which) {
    Fx h(VConfig());
    FxBank fb;
    TEvent outer;
    outer.id = HashId("Outer");
    TTrack t;
    TClip c;
    c.kind = 1;
    c.target = 1;  // the nested event's row (after its owner)
    t.clips.push_back(c);
    outer.tracks.push_back(t);
    fb.b.events.push_back(outer);
    fb.Event("Inner", 500.0f);
    h.Load("v", fb.Build("V"));
    SetDelay(h, which == 0 ? "Outer" : "Inner", 10.0f);
    h.Steps(1);
    const std::size_t f0 = h.Total();
    h.sys->Play(HashId("Outer"), NoEmitter(), nullptr);
    h.Steps(4);
    const bool wet = std::fabs(h.Mean(f0 + 8, f0 + kDelay)) < 1e-5f && h.LastMean(0) > 0.3f;
    std::printf("  [spec hole] setting on %s: the nested voice is %s\n", which == 0 ? "the played (outer) Event" : "the nested Event",
                wet ? "wet" : "dry");
    SND_CHECK(AllFinite(h.all));
  }
}

// Spec hole: snd_set_effect_param on a parameter marked [init] (read only by EffectInit).
SND_TEST(IndepG2Impl, SpecHole_InitOnlyParameterAtRunTime) {
  Fx h(G2Config());
  FxBank fb;
  fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, {5.0f, 0.0f, 0.0f, 1.0f, 10.0f})};
  fb.Event("D", 2000.0f + 64.0f);
  h.Load("v", fb.Build("V"));
  h.errors.clear();
  h.sys->SetEffectParam(HashId("Sfx"), 0, 4, 1000.0f, 0.0f);  // MaxDelay [init]
  h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 100.0f, 0.0f);   // Delay 100 ms: clamped to MaxDelay 10?
  h.Steps(2);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("D"), NoEmitter(), nullptr);
  h.Steps(24);
  const std::size_t on = FirstAbove(h, f0, 0.1f);
  std::printf("  [spec hole] MaxDelay set at run time: %llu warnings; the echo comes after %llu frames (480 = MaxDelay "
              "kept, 4800 = taken)\n",
              static_cast<unsigned long long>(h.errors.size()), static_cast<unsigned long long>(on == static_cast<std::size_t>(-1) ? 0 : on - f0));
  SND_CHECK(AllFinite(h.all));
}

// Spec hole: the per-playback value table (voice_effect_settings entries) is full.
SND_TEST(IndepG2Impl, SpecHole_PlaybackValueTableFull) {
  snd_config c = VConfig();
  c.voice_effect_settings = 2;
  Fx h(c);
  FxBank fb;
  fb.Event("A", 500.0f);
  h.Load("v", fb.Build("V"));
  SetDelay(h, "A", 10.0f);
  h.Steps(1);
  snd_playback_handle p[3];
  for (auto& x : p) x = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(4);
  h.errors.clear();
  for (auto& x : p) SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(x, 0, 3, 0.0f), SND_OK);  // WetMix 0
  h.Steps(4);
  std::printf("  [spec hole] third playback's value with a 2-entry table: level %.4f (0 = applied, %.4f = dropped), "
              "%llu warnings (code %d)\n",
              h.LastMean(0), 0.5f * kCenter, static_cast<unsigned long long>(h.errors.size()), h.errors.empty() ? 0 : h.errors[0].code);
  SND_CHECK(AllFinite(h.all));
}
