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

// Decision (re-check 1, sonus.h): a voice reads its Event's setting (kind and values together)
// every time it takes a voice-effect slot: at its start and when it comes back from virtual.
// A voice that started with a 10 ms delay, went virtual, and comes back after the Event was set
// to 30 ms sounds with 30 ms.
SND_TEST(IndepG2Impl, VoiceBackFromVirtualReadsTheEventSettingAsItIsNow) {
  VirtualRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 10.0f), SND_OK);
  r.h.Steps(1);
  r.h.sys->Play(HashId("A"), NoEmitter(), nullptr);  // wet, 10 ms
  r.h.Steps(4);
  const snd_playback_handle hp = r.h.sys->Play(HashId("H"), NoEmitter(), nullptr);  // A -> virtual
  r.h.Steps(6);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 30.0f), SND_OK);
  r.h.Steps(2);
  r.h.errors.clear();
  const std::size_t f0 = r.h.Total();
  r.h.sys->Stop(hp, 0.0f, nullptr);  // A back to real
  r.h.Steps(12);
  // The voice comes back with the block after the stop (R = f0 + 256, short fade-in). 30 ms:
  // silent until R + 1440, then the voice (fresh state).
  SND_CHECK(r.h.MaxAbs(f0 + 256, f0 + 256 + 1435) < 1e-5f);
  SND_CHECK_NEAR(r.h.Mean(f0 + 256 + 1440 + 300, f0 + 256 + 2400), 0.5f * kCenter, 1e-3);
  SND_CHECK_EQ(r.h.errors.size(), 0u);
}

// The kind changes while the voice is virtual: the returning voice gets the new kind with the
// new values (never the old kind with the new values). Echo 10 ms -> Probe (gain 0.5).
SND_TEST(IndepG2Impl, KindChangedWhileVirtualComesBackWithTheNewKindAndItsValues) {
  Log().Reset();
  const snd_dsp_effect probe = ProbePlugin();
  snd_config c = VConfig(4, 1);
  c.plugins.effect_count = 1;
  c.plugins.effects = &probe;
  VirtualRig r(c);
  SND_REQUIRE(r.h.sys != nullptr);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 10.0f), SND_OK);
  r.h.Steps(1);
  r.h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  r.h.Steps(4);
  const snd_playback_handle hp = r.h.sys->Play(HashId("H"), NoEmitter(), nullptr);
  r.h.Steps(6);
  const float half = 0.5f;
  SND_REQUIRE_EQ(r.h.sys->SetEventVoiceEffect(HashId("A"), 0, kProbeId, &half, 1), SND_OK);
  r.h.Steps(2);
  SND_CHECK_EQ(Log().Count(), 0u);
  const std::size_t f0 = r.h.Total();
  r.h.sys->Stop(hp, 0.0f, nullptr);
  r.h.Steps(8);
  SND_REQUIRE(Log().Count() > 0u);
  SND_CHECK_EQ(Log().calls[0].start0, 0.5f);
  SND_CHECK_EQ(Log().calls[0].end0, 0.5f);
  // No delay any more: heard at once (return at f0 + 256, fade-in 240 frames), at half the level.
  SND_CHECK(r.h.MaxAbs(f0 + 300, f0 + 400) > 0.05f);
  SND_CHECK_NEAR(r.h.Mean(f0 + 600, f0 + 900), 0.25f * kCenter, 1e-4);
  SND_CHECK_NEAR(r.h.LastMean(0), 0.25f * kCenter, 1e-5);
}

// Several round trips virtual <-> real, the Event's stage changed / removed / set again in
// between: each return reads the setting as it is then.
SND_TEST(IndepG2Impl, EveryReturnReadsTheSettingAgain) {
  VirtualRig r;
  SND_REQUIRE(r.h.sys != nullptr);
  SND_REQUIRE_EQ(SetDelay(r.h, "A", 10.0f), SND_OK);
  r.h.Steps(1);
  r.h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  r.h.Steps(4);
  r.h.errors.clear();
  // set: the Event's stage before the return; delay: expected onset after the return (0 = dry).
  struct Trip {
    int set;  // 0 = 20 ms, 1 = removed, 2 = 5 ms, 3 = unchanged
    std::size_t onset;
  };
  const Trip trips[] = {{0, 960}, {1, 0}, {2, 240}, {3, 240}};
  for (const Trip& t : trips) {
    const snd_playback_handle hp = r.h.sys->Play(HashId("H"), NoEmitter(), nullptr);
    r.h.Steps(6);
    if (t.set == 0) SetDelay(r.h, "A", 20.0f);
    if (t.set == 1) r.h.sys->SetEventVoiceEffect(HashId("A"), 0, SND_ID_NONE, nullptr, 0);
    if (t.set == 2) SetDelay(r.h, "A", 5.0f);
    r.h.Steps(2);
    const std::size_t f0 = r.h.Total();
    r.h.sys->Stop(hp, 0.0f, nullptr);
    r.h.Steps(8);
    const float L = 0.5f * kCenter;
    const std::size_t ret = f0 + 256;  // back to real with the block after the stop
    if (t.onset == 0) {
      SND_CHECK_MSG(std::fabs(r.h.Mean(ret + 300, ret + 600) - L) < 1e-4f, "set %d: dry expected, %.4f", t.set,
                    r.h.Mean(ret + 300, ret + 600));
      SND_CHECK(r.h.MaxAbs(ret + 2, ret + 100) > 0.0f);  // heard at once (fading in)
    } else {
      SND_CHECK_MSG(r.h.MaxAbs(ret, ret + t.onset - 2) < 1e-5f, "set %d: early output %.5f", t.set,
                    r.h.MaxAbs(ret, ret + t.onset - 2));
      SND_CHECK_MSG(std::fabs(r.h.Mean(ret + t.onset + 300, ret + t.onset + 600) - L) < 1e-3f, "set %d: level %.4f",
                    t.set, r.h.Mean(ret + t.onset + 300, ret + t.onset + 600));
    }
  }
  SND_CHECK_EQ(r.h.errors.size(), 0u);
}

// Decision: a stage removed while the voice is virtual is absent when it comes back (dry, no
// warning, not counted as dry for want of a slot).
SND_TEST(IndepG2Impl, StageRemovedWhileVirtualComesBackDryWithoutAWarning) {
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
  const std::uint64_t dry0 = r.h.sys->GetVoiceFxStats().dry;
  const std::size_t f0 = r.h.Total();
  r.h.sys->Stop(hp, 0.0f, nullptr);
  r.h.Steps(10);
  SND_CHECK(r.h.MaxAbs(f0 + 256 + 2, f0 + 256 + 100) > 0.0f);  // dry: heard at once after the return
  SND_CHECK_NEAR(r.h.Mean(f0 + 600, f0 + 900), 0.5f * kCenter, 1e-4);
  SND_CHECK_EQ(r.h.errors.size(), 0u);
  SND_CHECK_EQ(r.h.sys->GetVoiceFxStats().dry, dry0);
  SND_CHECK_EQ(r.h.sys->GetVoiceFxStats().slotsInUse, 0u);
}

// snd_set_playback_effect_param: a voice that takes a slot later uses the value from its first
// block (was Defect_PlaybackValueRampsFromTheEventValueWhenTheVoiceBecomesReal, fixed in 192bace).
SND_TEST(IndepG2Impl, PlaybackValueAppliesFromTheFirstBlockWhenTheVoiceBecomesReal) {
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

// sonus.h: snd_set_event_voice_effect records its 1 + param_count commands all or none
// (was Defect_EventVoiceEffectRefusedForAFullArenaIsPartlyApplied, fixed in 192bace).
SND_TEST(IndepG2Impl, EventVoiceEffectRefusedForAFullArenaIsNotApplied) {
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

// Decision: voices of a nested Event (CLIP kind 1) use the nested Event's setting.
SND_TEST(IndepG2Impl, NestedEventVoicesTakeTheNestedEventsSetting) {
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
    SND_CHECK_MSG(wet == (which == 1), "setting on the %s Event: the nested voice is %s", which == 0 ? "outer" : "nested",
                  wet ? "wet" : "dry");
  }
}

// Decision: an [init] parameter (kParamInitOnly, e.g. Echo MaxDelay) changed at run time is
// dropped with one warning SND_ERR_INVALID_ARGUMENT per command when applied (object = bus id for
// snd_set_effect_param, playback for snd_set_playback_effect_param); other parameters of the same
// commands' kinds still apply; as an snd_set_event_voice_effect value it is valid.
SND_TEST(IndepG2Impl, InitOnlyParameterAtRunTimeWarnsAndIsDropped) {
  Fx h(VConfig());
  FxBank fb;
  fb.b.buses[0].effects = {fb.Effect(dsp::kEffectEcho, {5.0f, 0.0f, 0.0f, 1.0f, 10.0f})};
  fb.Event("D", 2000.0f + 64.0f);
  fb.Event("A", 500.0f, 1);  // on the master: not through the bus echo
  h.Load("v", fb.Build("V"));
  h.errors.clear();
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 4, 1000.0f, 0.0f), SND_OK);  // MaxDelay [init]
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 4, 500.0f, 0.1f), SND_OK);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 100.0f, 0.0f), SND_OK);  // Delay: clamped to MaxDelay 10
  h.Steps(2);
  std::uint32_t bus = 0, other = 0;
  for (const Err& e : h.errors) {
    if (e.code == SND_ERR_INVALID_ARGUMENT && e.object == HashId("Sfx")) ++bus;
    else ++other;
  }
  SND_CHECK_EQ(bus, 2u);
  SND_CHECK_EQ(other, 0u);
  const std::size_t f0 = h.Total();
  h.sys->Play(HashId("D"), NoEmitter(), nullptr);
  h.Steps(24);
  // MaxDelay kept at 10 ms: Delay 100 clamped to it (one frame more, see the dsp Defect_ on the clamp).
  const std::size_t on = FirstAbove(h, f0, 0.1f);
  SND_CHECK(on == f0 + 480 || on == f0 + 481);
  // Voice side: MaxDelay as an Event value is valid (sizes the state at the slot's init).
  h.errors.clear();
  const float p[] = {40.0f, 0.0f, 0.0f, 1.0f, 40.0f};
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 0, dsp::kEffectEcho, p, 5), SND_OK);
  h.Steps(1);
  const std::size_t f1 = h.Total();
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(12);
  SND_CHECK_EQ(FirstAbove(h, f1, 0.1f), f1 + 1920);  // 40 ms
  SND_CHECK_EQ(h.errors.size(), 0u);
  // ... but not as a per-playback value: one warning (object = playback), value dropped.
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(a, 0, 4, 1.0f), SND_OK);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(a, 0, 3, 0.5f), SND_OK);  // WetMix: applies
  h.Steps(4);
  std::uint32_t pb = 0;
  other = 0;
  for (const Err& e : h.errors) {
    if (e.code == SND_ERR_INVALID_ARGUMENT && e.object == a.value) ++pb;
    else ++other;
  }
  SND_CHECK_EQ(pb, 1u);
  SND_CHECK_EQ(other, 0u);
  SND_CHECK_NEAR(h.LastMean(0), 0.25f * kCenter, 1e-5);
  SND_CHECK(AllFinite(h.all));
}

// Plugins have no [init] flags: every parameter of a plugin bus effect / voice effect changes at
// run time without a warning.
SND_TEST(IndepG2Impl, PluginParametersAreNeverInitOnly) {
  Log().Reset();
  const snd_dsp_effect probe = ProbePlugin();
  snd_config c = VConfig();
  c.plugins.effect_count = 1;
  c.plugins.effects = &probe;
  Fx h(c);
  SND_REQUIRE(h.sys != nullptr);
  FxBank fb;
  fb.b.buses[0].effects = {fb.Effect(kProbeId, {1.0f, 0.0f, 0.0f, 0.0f, 0.0f})};
  fb.Event("A", 500.0f);
  h.Load("v", fb.Build("V"));
  const float g[] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  SND_REQUIRE_EQ(h.sys->SetEventVoiceEffect(HashId("A"), 0, kProbeId, g, 5), SND_OK);
  h.Steps(1);
  const snd_playback_handle a = h.sys->Play(HashId("A"), NoEmitter(), nullptr);
  h.Steps(2);
  h.errors.clear();
  h.sys->SetEffectParam(HashId("Sfx"), 0, 4, 3.0f, 0.0f);
  h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 0.5f, 0.0f);
  h.sys->SetPlaybackEffectParam(a, 0, 4, 3.0f);
  h.sys->SetPlaybackEffectParam(a, 0, 0, 0.5f);
  h.Steps(4);
  SND_CHECK_EQ(h.errors.size(), 0u);
  SND_CHECK_NEAR(h.LastMean(0), 0.5f * 0.25f * kCenter, 1e-5);  // DC 0.5 x bus 0.5 x voice 0.5
}

// Decision: the per-playback value table full: warning SND_ERR_LIMIT_REACHED (object = the
// playback) when applied, value dropped.
SND_TEST(IndepG2Impl, PlaybackValueTableFullWarnsAndDrops) {
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
  SND_CHECK_NEAR(h.LastMean(0), 0.5f * kCenter, 1e-5);  // one of the three still at WetMix 1
  SND_REQUIRE_EQ(h.errors.size(), 1u);
  SND_CHECK_EQ(h.errors[0].code, SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(h.errors[0].object, p[2].value);
}
