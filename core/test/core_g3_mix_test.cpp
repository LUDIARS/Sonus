// Stage 3 group 3. Measure relative changes to a steady mono source.
#include <cmath>
#include <ctime>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"

using namespace coretest;
using snd::HashId;

namespace {
TestBank SignalBank() {
  auto b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(48000, 0.1f), 0, 48000});
  b.events.push_back(SimpleEvent("E", 0));
  b.events[0].tracks[0].clips[0].loopCount = 0u;
  return b;
}
float Level(Harness& h) {
  h.Steps(4);
  return h.out.back();
}
}

SND_TEST(CoreG3, ForbiddenScopeKeepsAllowedValue) {
  auto b = SignalBank();
  b.params.push_back(TParam{HashId("P"), 0, -24, 0, 0, 0, 1});
  b.curves.push_back(TCurve{HashId("P"), 0, 0, {{-24,-24}, {-6,-6}, {0,0}}});
  b.events[0].curves = {0};
  Harness h;
  h.Load("b", b.Build(HashId("B")));
  const auto em = h.sys->CreateEmitter();
  const auto p = h.sys->Play(HashId("E"), em, nullptr);
  const auto base = Level(h);
  SND_CHECK(base > 0.0f);
  h.sys->SetParamGlobal(HashId("P"), -6.0f);
  h.sys->SetParamEmitter(em, HashId("P"), -18.0f);
  h.sys->SetParamPlayback(p, HashId("P"), -24.0f);
  SND_CHECK_NEAR(Level(h), base * std::pow(10.0f, -0.3f), 0.00001f);
}

SND_TEST(CoreG3, SoloSubtreeAndFocusRestoreIndependentMute) {
  auto cfg = SmallConfig();
  cfg.focus_mode = SND_FOCUS_PER_CATEGORY;
  Harness h(cfg);
  auto b = SignalBank();
  b.categories = {{HashId("child"),1,0,0}, {HashId("parent"),kNone,bf::kCategoryMuteOnFocusLoss,0},
                  {HashId("other"),kNone,0,0}};
  b.events[0].category = 0;
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("E"), {}, nullptr);
  const float base = Level(h);
  SND_CHECK(base > 0.0f);
  h.sys->SetCategorySolo(HashId("other"), true);
  SND_CHECK_EQ(Level(h), 0.0f);
  h.sys->SetCategorySolo(HashId("parent"), true);
  SND_CHECK_NEAR(Level(h), base, 1e-6f);
  h.sys->SetFocusLost(true);
  SND_CHECK_EQ(Level(h), 0.0f);
  h.sys->MuteCategory(HashId("child"), true);
  h.sys->SetFocusLost(false);
  SND_CHECK_EQ(Level(h), 0.0f);
  h.sys->MuteCategory(HashId("child"), false);
  SND_CHECK_NEAR(Level(h), base, 1e-6f);
}

SND_TEST(CoreG3, SnapshotSendOffsetsStackAndRelease) {
  auto b = SignalBank();
  b.buses[0].parent = kNone;
  b.buses[0].flags = bf::kBusOutput;
  b.buses[0].sends = {{1u, -6.0f, 1u}};
  b.snapshots = {{HashId("A"),{{2,0,0,12}}}, {HashId("B"),{{2,0,0,-12}}}};
  Harness h;
  h.Load("b", b.Build(HashId("SendBank")));
  h.sys->Play(HashId("E"), {}, nullptr);
  const float base = Level(h);
  SND_CHECK(base > 0.0f);
  h.sys->ApplySnapshot(HashId("A"), 0.5f, 0);
  SND_CHECK_NEAR(Level(h), base * 1.9952623f, 1e-5f);
  h.sys->ApplySnapshot(HashId("B"), 0.5f, 0);
  SND_CHECK_NEAR(Level(h), base, 1e-6f);
  h.sys->ApplySnapshot(HashId("A"), 0.0f, 0);
  SND_CHECK_NEAR(Level(h), base / 1.9952623f, 1e-5f);
}
