// Group 4 recheck. Observe output frames and notifications through the independent rig.
#include "indep_g4_fixture.hpp"

using namespace g4;

namespace {

TClip Nested(std::uint32_t eventRow) {
  TClip clip;
  clip.kind = 1;
  clip.target = eventRow;
  return clip;
}

TAction Action(float ms, std::uint32_t kind, const char* target, float value = 0.0f) {
  TAction action;
  action.timeMs = ms;
  action.action = kind;
  action.target = HashId(target);
  action.value = value;
  return action;
}

}  // namespace

// There are three authorable layers. On either side of the triple overlap,
// isolate each clip and compare the power of the two audible clips.
SND_TEST(IndepG4Recheck, EqualPowerWhereOnlyTwoOfThreeLayersOverlap) {
  Rig r;
  B bank;
  TParam param;
  param.id = HashId("blend");
  param.maxValue = 1.0f;
  bank.b.params.push_back(param);
  const char* names[] = {"Left", "Middle", "Right"};
  const float begin[] = {0.0f, 0.2f, 0.45f};
  const float end[] = {0.55f, 0.8f, 1.0f};
  for (int solo = 0; solo < 3; ++solo) {
    std::vector<TClip> clips;
    for (int layer = 0; layer < 3; ++layer) {
      TClip c = B::SynthClip(bank.Synth(solo == layer ? 500.0f : 0.0f));
      c.layerMin = begin[layer];
      c.layerMax = end[layer];
      clips.push_back(c);
    }
    bank.Event(names[solo], clips, 7).tracks[0].layerParam = param.id;
  }
  r.Load("b", bank.Build("B"));
  for (float blend : {0.30f, 0.40f, 0.60f, 0.70f}) {
    r.sys->SetParamGlobal(param.id, blend);
    float gain[3] = {};
    for (int solo = 0; solo < 3; ++solo) {
      const auto playback = r.sys->Play(HashId(names[solo]), NoEm(), nullptr);
      r.Steps(4);
      gain[solo] = r.At(r.Now() - 1) / (0.5f * kC);
      r.sys->Stop(playback, 0.0f, nullptr);
      r.Steps(3);
    }
    const int outside = blend < begin[2] ? 2 : 0;
    SND_CHECK_NEAR(gain[outside], 0.0f, 1e-5f);
    SND_CHECK(gain[1] > 0.0f && gain[2 - outside] > 0.0f);
    SND_CHECK_NEAR(gain[0] * gain[0] + gain[1] * gain[1] + gain[2] * gain[2], 1.0f, 1e-3f);
  }
}

SND_TEST(IndepG4Recheck, PausedPitchedLoopFiresOnePulseAtItsTrackFrame) {
  for (float cents : {-1200.0f, 1200.0f}) {
    Rig r;
    B bank;
    TClip looping = B::AssetClip(bank.Asset("loop", Constant(192, 0.0f)));
    looping.loopCount = 0;
    looping.pitchCents = cents;
    bank.Event("Parent", {looping}).tracks[0].actions = {Action(15.0f, 0, "Pulse")};
    bank.Event("Pulse", {B::AssetClip(bank.Asset("click", Constant(32, 0.5f)))});
    r.Load("b", bank.Build("B"));
    const std::size_t start = r.Now();
    const auto playback = r.sys->Play(HashId("Parent"), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Pause(playback, 0.0f);
    r.Steps(4);
    r.sys->ResumePlayback(playback, 0.0f);
    r.Steps(16);
    const std::size_t expected = start + 4 * r.Frames() + 720;
    SND_CHECK_EQ(r.FirstAbove(start, r.Now(), 0.25f * kC), expected);
    SND_CHECK_EQ(r.CountAbove(start, r.Now(), 0.25f * kC), 32u);
    SND_CHECK_EQ(r.EndAbove(start, r.Now(), 0.25f * kC), expected + 32);
    SND_CHECK_EQ(r.CountEnds(playback.value), 0u);
  }
}

SND_TEST(IndepG4Recheck, RestartFadeCountsVirtualTimeAndExcludesPausedTime) {
  for (std::uint32_t pausedBlocks : {0u, 8u}) {
    Rig r(Config(1));
    B bank;
    auto& low = bank.Event("Low", {B::AssetClip(bank.Asset("dc", Constant(32000, 0.5f)))});
    low.priority = 100;
    low.virtualReturn = 1;
    bank.Event("High", {B::SynthClip(bank.Synth(0.0f))}).priority = 200;
    r.Load("b", bank.Build("B"));
    const std::size_t start = r.Now();
    const auto lowPlay = r.sys->PlayFade(HashId("Low"), NoEm(), nullptr, 0.1f);
    r.Steps(2);
    const auto highPlay = r.sys->Play(HashId("High"), NoEm(), nullptr);
    r.Steps(4);
    if (pausedBlocks != 0) {
      r.sys->Pause(lowPlay, 0.0f);
      r.Steps(pausedBlocks);
      r.sys->ResumePlayback(lowPlay, 0.0f);
    }
    r.Steps(4);
    SND_CHECK(r.sample.advances.load() > 0u);
    r.sys->Stop(highPlay, 0.0f, nullptr);
    r.Steps(3);
    const std::size_t pausedFrames = pausedBlocks * r.Frames();
    for (std::size_t f = r.Now() - r.Frames(); f < r.Now(); ++f) {
      const float expected = 0.5f * static_cast<float>(f - start - pausedFrames) / 4800.0f;
      SND_REQUIRE(std::fabs(r.At(f) / kC - expected) <= 0.5f / 4800.0f + 1e-5f);
    }
    SND_CHECK_EQ(r.CountEnds(lowPlay.value), 0u);
  }
}

SND_TEST(IndepG4Recheck, SurvivingVoiceDeterminesNaturalOrStoppedEnd) {
  for (bool stopSurvivor : {false, true}) {
    Rig r;
    B bank;
    bank.b.groups.push_back(TGroup{HashId("G"), 1, 0});
    bank.Event("Parent", {B::SynthClip(bank.Synth(100.0f)), Nested(1)}).group = 0;
    bank.Event("Child", {B::AssetClip(bank.Asset("finite", Constant(4096, 0.25f)))});
    bank.Event("Replacement", {B::SynthClip(bank.Synth(200.0f))}).group = 0;
    r.Load("b", bank.Build("B"));
    const auto parent = r.sys->Play(HashId("Parent"), NoEm(), nullptr);
    r.Steps(2);
    const auto next = r.sys->Play(HashId("Replacement"), NoEm(), nullptr);
    r.Steps(4);
    SND_CHECK_EQ(r.CountEnds(parent.value), 0u);
    SND_CHECK_NEAR(r.At(r.Now() - 1) / kC, 0.45f, 1e-4f);
    if (stopSurvivor) r.sys->Stop(parent, 0.0f, nullptr);
    r.Steps(16);
    SND_CHECK_EQ(r.CountEnds(parent.value), 1u);
    SND_CHECK(r.Ended(parent.value, stopSurvivor ? SND_END_STOPPED : SND_END_COMPLETED, SND_END_DETAIL_NONE));
    SND_CHECK_EQ(r.CountEnds(next.value), 0u);
    SND_CHECK_NEAR(r.At(r.Now() - 1) / kC, 0.20f, 1e-4f);
  }
}

// A final control action must survive its block quantization even with no audio clips.
SND_TEST(IndepG4Recheck, ActionOnlyParentAppliesFinalSnapshotBeforeEnding) {
  Rig r;
  B bank;
  TSnapshot snap;
  snap.id = HashId("Quiet");
  snap.values.push_back(TSnapshot::Value{0, 0, 0, -20.0f});
  bank.b.snapshots.push_back(snap);
  bank.Event("Signal", {B::SynthClip(bank.Synth(500.0f))});
  bank.Event("Controller", {}).tracks[0].actions = {Action(10.0f, 3, "Quiet", 1.0f)};
  r.Load("b", bank.Build("B"));
  r.sys->Play(HashId("Signal"), NoEm(), nullptr);
  r.Steps(3);
  const std::size_t start = r.Now();
  const auto controller = r.sys->Play(HashId("Controller"), NoEm(), nullptr);
  r.Steps(30);
  SND_CHECK_NEAR(r.At(start + 511) / kC, 0.5f, 1e-4f);
  SND_CHECK_NEAR(r.At(r.Now() - 1) / kC, 0.05f, 1e-3f);
  SND_REQUIRE(r.EndOf(controller.value) != nullptr);
  SND_CHECK(r.EndOf(controller.value)->block * r.Frames() >= start + 512);
  SND_CHECK_EQ(r.CountEnds(controller.value), 1u);
  SND_CHECK(r.Ended(controller.value, SND_END_COMPLETED, SND_END_DETAIL_NONE));
}

SND_TEST(IndepG4Recheck, TwoGroupsStealingTheLastVoicesEndPlaybackOnce) {
  for (bool sameBlock : {false, true}) {
    Rig r;
    B bank;
    bank.b.groups.push_back(TGroup{HashId("G0"), 1, 0});
    bank.b.groups.push_back(TGroup{HashId("G1"), 1, 0});
    bank.Event("Parent", {B::SynthClip(bank.Synth(200.0f)), Nested(1)}).group = 0;
    bank.Event("Child", {B::SynthClip(bank.Synth(300.0f))}).group = 1;
    bank.Event("ReplaceParent", {B::SynthClip(bank.Synth(100.0f))}).group = 0;
    bank.Event("ReplaceChild", {B::SynthClip(bank.Synth(50.0f))}).group = 1;
    r.Load("b", bank.Build("B"));
    const auto parent = r.sys->Play(HashId("Parent"), NoEm(), nullptr);
    r.Steps(3);
    SND_CHECK_NEAR(r.At(r.Now() - 1) / kC, 0.5f, 1e-4f);
    r.sys->Play(HashId("ReplaceParent"), NoEm(), nullptr);
    if (!sameBlock) {
      r.Steps(3);
      SND_CHECK_EQ(r.CountEnds(parent.value), 0u);
      SND_CHECK_NEAR(r.At(r.Now() - 1) / kC, 0.4f, 1e-4f);
    }
    r.sys->Play(HashId("ReplaceChild"), NoEm(), nullptr);
    r.Steps(6);
    SND_CHECK_EQ(r.CountEnds(parent.value), 1u);
    SND_CHECK(r.Ended(parent.value, SND_END_STOLEN, SND_END_DETAIL_LIMIT_GROUP));
    SND_CHECK_NEAR(r.At(r.Now() - 1) / kC, 0.15f, 1e-4f);
  }
}
