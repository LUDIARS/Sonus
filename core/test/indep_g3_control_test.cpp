#include "indep_g2_fixture.hpp"

namespace {
using namespace g2;
using snd::HashId;

SND_TEST(IndepG3Category, ParentAndLeafSolosCombineAndClear) {
  Fx h(G2Config());
  FxBank bank;
  bank.b.categories = {{HashId("Leaf"), 2, 0, 0}, {HashId("Peer"), kNone, 0, 0},
                       {HashId("Parent"), kNone, 0, 0}};
  bank.Event("One", 200).category = 0;
  bank.Event("Two", 400).category = 1;
  h.Load("solo", bank.Build("Solo"));
  h.sys->Play(HashId("One"), NoEmitter(), nullptr);
  h.sys->Play(HashId("Two"), NoEmitter(), nullptr);
  h.Steps(8);
  const float both = h.LastMean();
  SND_CHECK_NEAR(both, 0.6f * kCenter, 0.001f);
  h.sys->SetCategorySolo(HashId("Parent"), true);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), both / 3, 1e-4);
  h.sys->SetCategorySolo(HashId("Peer"), true);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), both, 1e-4);
  h.sys->SetCategorySolo(HashId("Parent"), false);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), both * 2 / 3, 1e-4);
  h.sys->SetCategorySolo(HashId("Peer"), false);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), both, 1e-4);
}

SND_TEST(IndepG3Category, RegainingFocusPreservesExplicitMute) {
  auto cfg = G2Config();
  cfg.focus_mode = SND_FOCUS_PER_CATEGORY;
  Fx h(cfg);
  FxBank bank;
  bank.b.categories = {{HashId("Menu"), kNone, 0, 0},
                       {HashId("Game"), kNone, bf::kCategoryMuteOnFocusLoss, 0}};
  bank.Event("One", 200).category = 0;
  bank.Event("Two", 400).category = 1;
  h.Load("focus", bank.Build("Focus"));
  h.sys->Play(HashId("One"), NoEmitter(), nullptr);
  auto game = h.sys->Play(HashId("Two"), NoEmitter(), nullptr);
  h.Steps(8);
  h.sys->SetFocusLost(true);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), 0.2f * kCenter, 1e-4);
  snd_position a{}, b{};
  SND_REQUIRE_EQ(h.sys->GetPosition(game, &a), SND_OK);
  h.sys->MuteCategory(HashId("Menu"), true);
  h.Steps(8);
  SND_CHECK_NEAR(h.LastMean(), 0, 1e-6);
  SND_REQUIRE_EQ(h.sys->GetPosition(game, &b), SND_OK);
  SND_CHECK(b.frames > a.frames);
  h.sys->SetFocusLost(false);
  h.Steps(4);
  SND_CHECK_NEAR(h.LastMean(), 0.4f * kCenter, 1e-4);
}

SND_TEST(IndepG3Pause, IgnoreFlagsSurviveGlobalPauseButIndividualPauseStillStops) {
  for (const bool eventFlag : {false, true}) {
    Fx h(G2Config());
    FxBank bank;
    bank.b.categories = {{HashId("Always"), kNone, eventFlag ? 0u : bf::kCategoryIgnorePause, 0},
                         {HashId("Ordinary"), kNone, 0, 0}};
    auto& always = bank.Event("One", 200);
    always.category = 0;
    always.flags = eventFlag ? bf::kEventIgnorePause : 0u;
    bank.Event("Two", 400).category = 1;
    h.Load("pause", bank.Build("Pause"));
    auto one = h.sys->Play(HashId("One"), NoEmitter(), nullptr);
    h.sys->Play(HashId("Two"), NoEmitter(), nullptr);
    h.Steps(8);
    h.sys->PauseCategory(SND_ID_NONE, true);
    h.Steps(8);
    SND_CHECK_NEAR(h.LastMean(), 0.2f * kCenter, 1e-4);
    h.sys->Pause(one, 0);
    h.Steps(8);
    SND_CHECK_NEAR(h.LastMean(), 0, 1e-6);
    h.sys->ResumePlayback(one, 0);
    h.Steps(8);
    SND_CHECK_NEAR(h.LastMean(), 0.2f * kCenter, 1e-4);
  }
}

void ActionFixture(Fx& h, float timeMs) {
  FxBank bank;
  bank.b.groups.push_back({HashId("Limited"), 1, 3});
  bank.b.snapshots.push_back({HashId("Quiet"), {{0, 0, 0, -24}}});
  bank.Event("Signal", 360);
  bank.Event("Occupy", 0).group = 0;
  auto& parent = bank.Event("Parent", 0);
  parent.group = 0;
  parent.tracks[0].actions = {{timeMs, 3, HashId("Quiet"), 1}};
  h.Load("actions", bank.Build("Actions"));
  h.sys->SetSnapshotActionTransition(HashId("Quiet"), 0.128f);
  h.sys->Play(HashId("Signal"), NoEmitter(), nullptr);
  h.Steps(8);
}

SND_TEST(IndepG3Action, AcceptedInitialActionUsesTransitionDuration) {
  Fx h(G2Config());
  ActionFixture(h, 0);
  const float base = h.LastMean();
  SND_REQUIRE(base > 0.2f);
  const auto start = h.Total();
  h.sys->Play(HashId("Parent"), NoEmitter(), nullptr);
  h.Steps(32);
  // 128 ms = 24 blocks. 6 and 18 blocks into a -24 dB transition.
  // Allow one block of block-rate publication/interpolation.
  const auto dbAt = [&](unsigned blocks) { return Db(h.At(start + blocks * h.Frames()) / base); };
  SND_CHECK_NEAR(dbAt(6), -6, 1.1);
  SND_CHECK_NEAR(dbAt(18), -18, 1.1);
  SND_CHECK_NEAR(Db(h.LastMean() / base), -24, 0.05);
}

SND_TEST(IndepG3Action, RejectedInitialActionPreservesAlreadyRunningTransition) {
  Fx h(G2Config());
  ActionFixture(h, 0);
  const float base = h.LastMean();
  h.sys->Play(HashId("Occupy"), NoEmitter(), nullptr);
  h.sys->ApplySnapshot(HashId("Quiet"), 0.5f, 0.128f);
  h.Steps(6);
  const auto rejected = h.sys->Play(HashId("Parent"), NoEmitter(), nullptr);
  h.Steps(8);
  SND_CHECK_NEAR(Db(h.LastMean() / base), -7, 1.1);
  h.Steps(32);
  SND_CHECK(h.Ended(rejected.value, SND_END_REJECTED));
  SND_CHECK_NEAR(Db(h.LastMean() / base), -12, 0.05);
}

SND_TEST(IndepG3Action, PausingBeforeFutureActionDelaysItsTransition) {
  Fx h(G2Config());
  ActionFixture(h, 64);
  const float base = h.LastMean();
  const auto parent = h.sys->Play(HashId("Parent"), NoEmitter(), nullptr);
  h.Steps(4);
  h.sys->Pause(parent, 0);
  h.Steps(24);
  SND_CHECK_NEAR(h.LastMean(), base, 1e-4);
  h.sys->ResumePlayback(parent, 0);
  h.Steps(3);
  SND_CHECK_NEAR(h.LastMean(), base, 1e-4);
  h.Steps(17);
  const double db = Db(h.LastMean() / base);
  SND_CHECK_MSG(db < -5 && db > -19, "level after resume %.3f dB", db);
  h.Steps(28);
  SND_CHECK_NEAR(Db(h.LastMean() / base), -24, 0.05);
}
} // namespace
