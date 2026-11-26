// The preview engine (group P, tool/preview): plays the document's build in the tool's process.
// Analytic checks on the output (the DC material of live_fixture.hpp): levels follow the bus
// volumes, the 3D preview's distance and pan (SpatialPreview, the model the runtime compare test
// holds to the core), live edits act on the sound, stop-all silences, a music transition audition
// switches at the block MusicEditor::Preview computes.
#include <cmath>

#include "live_fixture.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace snd::tool;
using snd::tool::preview::PreviewTransition;
using livetest::kDc;
using livetest::Ratio;
using livetest::Rig;

SND_TEST(LivePreview, PlaysTheDocumentAndStopsAll) {
  Rig r;
  SND_REQUIRE(r.ok);
  const PreviewStatus st = r.engine->Status();
  SND_CHECK(st.running);
  SND_CHECK_EQ(st.banks, 1u);
  SND_CHECK_EQ(st.buildHash, r.gameBuild.buildHash);
  SND_CHECK_EQ(r.engine->projectBuildHash(), r.gameBuild.buildHash);
  r.Frames(2);
  SND_CHECK_EQ(r.Left(), 0.0f);
  const std::uint64_t tone = r.engine->Play(snd::HashId("Tone"), nullptr);
  SND_REQUIRE(tone != 0u);
  r.Frames(8);
  const float base = r.Left();
  SND_CHECK(base > 0.05f);
  SND_CHECK_NEAR(r.Left(), r.Right(), 1e-6);
  // A second sound on another bus adds the same DC at that bus's volume (Music -3 dB).
  SND_REQUIRE(r.engine->Play(snd::HashId("Pad"), nullptr) != 0u);
  r.Frames(8);
  SND_CHECK_NEAR(r.Left(), base * (1.0 + Ratio(-3.0)), 1e-5);
  SND_CHECK_EQ(r.engine->Status().activePlaybacks, 2u);
  SND_CHECK_EQ(r.engine->StopAll(), 2u);
  r.Frames(4);  // the short fade (5 ms) ends inside the next block
  SND_CHECK_EQ(r.Left(), 0.0f);
  SND_CHECK_EQ(r.Right(), 0.0f);
  SND_CHECK_EQ(r.engine->Status().activePlaybacks, 0u);
  // An unknown event: the runtime takes the request and ends the playback (nothing plays).
  const std::uint64_t nope = r.engine->Play(snd::HashId("Nope"), nullptr);
  r.Frames(70);
  SND_CHECK_EQ(r.Left(), 0.0f);
  if (nope != 0u) SND_CHECK_EQ(r.engine->Status().activePlaybacks, 0u);
}

SND_TEST(LivePreview, ThreeDPreviewMatchesTheModel) {
  Rig r;
  SND_REQUIRE(r.ok);
  AuditionRouter router;
  router.Bind(&r.conn, r.engine.get());
  router.SetTarget(AuditionTarget::Preview);
  SpatialPreview prev(r.doc, livetest::G(12));
  SND_REQUIRE(prev.valid());
  prev.MoveEmitter(-3.0f, 4.0f);  // 5 m, front-left
  const AuditionResult a = router.Start3D(prev.Audition());
  SND_REQUIRE(a.ok);
  SND_CHECK(a.unsupported.empty());
  r.Frames(8);
  SpatialResult want = prev.Compute(2, 1);
  SND_CHECK_NEAR(r.Left(), kDc * want.outputGain[0], 2e-5);
  SND_CHECK_NEAR(r.Right(), kDc * want.outputGain[1], 2e-5);
  SND_CHECK(r.Left() > r.Right());  // to the left
  // Moving the emitter while it plays (3D preview): to the right, nearer.
  prev.MoveEmitter(2.0f, 1.0f);
  SND_REQUIRE(router.Update3D(prev.listener(), prev.emitter()).ok);
  r.Frames(8);
  want = prev.Compute(2, 1);
  SND_CHECK_NEAR(r.Left(), kDc * want.outputGain[0], 2e-5);
  SND_CHECK_NEAR(r.Right(), kDc * want.outputGain[1], 2e-5);
  SND_CHECK(r.Right() > r.Left());
  // Beyond maxDistance (12 m): silent, as the model says.
  prev.MoveEmitter(9.0f, 12.0f);
  SND_REQUIRE(router.Update3D(prev.listener(), prev.emitter()).ok);
  r.Frames(8);
  want = prev.Compute(2, 1);
  SND_CHECK_NEAR(r.Left(), kDc * want.outputGain[0], 2e-5);
  SND_CHECK(router.StopAll().ok);
  r.Frames(3);
  SND_CHECK_EQ(r.Left(), 0.0f);
}

SND_TEST(LivePreview, EditsActOnThePreviewThroughTheLiveLink) {
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  SND_REQUIRE(r.engine->Play(snd::HashId("Tone"), nullptr) != 0u);
  r.Frames(8);
  const double base = r.Left();
  SND_REQUIRE(base > 0.05);
  SND_REQUIRE(r.doc.SetNumber(livetest::SfxGuid(), "volumeDb", -6.0));
  SND_REQUIRE(r.Settled());
  SND_CHECK_NEAR(r.Left() / base, Ratio(-6.0), 1e-5);
  const LiveItem* it = r.sync->Find(livetest::SfxGuid(), "volumeDb");
  SND_REQUIRE(it != nullptr);
  SND_CHECK(it->route == LiveRoute::SetValue);
  SND_CHECK(it->state == LiveState::Live);
}

SND_TEST(LivePreview, TransitionAuditionSwitchesAtTheComputedBlock) {
  Rig r(true);
  SND_REQUIRE(r.ok);
  MusicEditor me(r.doc, livetest::G(26));
  SND_REQUIRE(me.valid());
  MusicSettings ms;
  ms.sampleRate = r.engine->Status().sampleRate;
  ms.blockFrames = r.engine->Status().blockFrames;
  const double request = 1.1;  // seconds after the start
  const TransitionPreview p = me.Preview("Explore", "Battle", request, ms);
  SND_REQUIRE(p.valid);
  SND_CHECK_NEAR(p.fireSeconds, 2.0, 1e-9);  // the next bar at 120 BPM 4/4
  AuditionRouter router;
  router.Bind(&r.conn, r.engine.get());
  r.Frames(3);
  const std::uint64_t startBlock = r.engine->NextBlock();
  TransitionAudition t{snd::HashId("Song"), snd::HashId("Explore"), snd::HashId("Battle"), request};
  const AuditionResult a = router.PlayTransition(t);
  SND_REQUIRE(a.ok);
  // Render 3 s; the transition must happen once, at the computed sample.
  std::vector<float> all;
  const std::uint32_t frames = ms.blockFrames;
  const std::uint32_t blocks = static_cast<std::uint32_t>(3.0 * ms.sampleRate / frames);
  SND_REQUIRE_EQ(r.engine->Render(blocks, &all), blocks);
  const std::vector<PreviewTransition> tr = r.engine->Transitions();
  SND_REQUIRE(!tr.empty());
  const PreviewTransition& last = tr.back();
  SND_CHECK_EQ(last.to, snd::HashId("Battle"));
  const std::uint64_t startClock = startBlock * frames;
  const std::uint64_t want = startClock + static_cast<std::uint64_t>(std::llround(p.fireSeconds * ms.sampleRate));
  SND_CHECK_EQ(last.clock / frames, want / frames);  // the computed block
  SND_CHECK_EQ(last.clock, want);                     // and sample
  // The output: SegA's level before, SegB's (twice) after.
  const std::uint32_t ch = r.engine->Status().channels;
  auto at = [&](std::uint64_t clock) { return all[static_cast<std::size_t>(clock - startClock) * ch]; };
  const float before = at(want - frames), after = at(want + frames);
  SND_CHECK(before > 0.01f);
  SND_CHECK_NEAR(after / before, 2.0, 1e-4);
  // Parameter and state while auditioning: back to Explore at the next bar (one more transition).
  SND_CHECK(router.SetParam(snd::HashId("Knob"), 0.75f).ok);
  SND_CHECK(router.SetState(snd::HashId("Song"), snd::HashId("Explore")).ok);
  r.engine->Render(static_cast<std::uint32_t>(2.5 * ms.sampleRate / frames), nullptr);
  const std::vector<PreviewTransition> tr2 = r.engine->Transitions();
  SND_REQUIRE(tr2.size() == tr.size() + 1u);
  SND_CHECK_EQ(tr2.back().to, snd::HashId("Explore"));
  SND_CHECK_EQ((tr2.back().clock - startClock) % static_cast<std::uint64_t>(2.0 * ms.sampleRate), 0u);  // a bar line
  SND_CHECK(router.StopAll().ok);
}

SND_TEST(LivePreview, AuditionsToAConnectedGame) {
  // The preview engine stands in for a game: the router sends live link auditions.
  Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  AuditionRouter router;
  router.Bind(&r.conn, r.engine.get());
  router.SetTarget(AuditionTarget::Game);
  std::string why;
  SND_CHECK(router.Ready(&why));
  AuditionRequest q;
  q.eventId = snd::HashId("Tone");
  q.atPosition = false;
  const AuditionResult a = router.PlayEvent(q);
  SND_REQUIRE(a.ok);
  SND_CHECK(a.seq != 0u);
  SND_REQUIRE(r.Run([&] { return r.Left() > 0.05f; }));
  // Moving a game audition: said, not done.
  const AuditionResult m = router.Update3D(ListenerPose{}, EmitterPose{});
  SND_CHECK(!m.ok);
  SND_CHECK(!m.unsupported.empty());
  // Stop-all (1.1): only the auditions; the game's own playback keeps playing.
  SND_REQUIRE(r.engine->Play(snd::HashId("Pad"), nullptr) != 0u);
  r.Frames(8);
  const float both = r.Left();
  const AuditionResult s = router.StopAll();
  SND_REQUIRE(s.ok);
  SND_REQUIRE(r.Run([&] { return r.Left() < both * 0.9f; }));
  r.Frames(8);
  SND_CHECK_NEAR(r.Left(), both * Ratio(-3.0) / (1.0 + Ratio(-3.0)), 1e-5);  // Pad alone (Music -3 dB)
  // Without a connection the router says so.
  r.conn.Close();
  SND_CHECK(!router.Ready(&why));
  SND_CHECK(!why.empty());
  SND_CHECK(!router.PlayEvent(q).ok);
}
