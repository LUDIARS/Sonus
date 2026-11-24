// Independent tests of group P: the preview engine and the audition router
// (tool/test/indep_s7_live_properties.md D). Levels are analytic: the DC material and the bus
// volumes, and for 3D the tool's SpatialPreview model (which tool_runtime_compare_test holds to
// core's arithmetic).
#include <cmath>
#include <limits>

#include "indep_s7_live_fixture.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace snd::tool;
using namespace ilive;

namespace {
bool Finite(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}

}  // namespace

// D1: bus volumes scale the DC exactly; two sounds add; the centre is the same on both sides.
SND_TEST(IndepS7LivePreview, BusVolumesScaleTheOutputExactly) {
  Engine en;
  SND_REQUIRE(en.e != nullptr);
  SND_REQUIRE(en.e->Play(snd::HashId("Tone"), nullptr) != 0u);  // Sfx -1 dB
  en.Frames(8);
  const double tone = en.L();
  SND_REQUIRE(tone > 0.01);
  SND_CHECK_EQ(en.L(), en.R());
  SND_REQUIRE(en.e->Play(snd::HashId("Hum"), nullptr) != 0u);  // Amb -2 dB
  en.Frames(8);
  SND_CHECK_NEAR(en.L(), tone * (1.0 + DbRatio(-1.0)), 1e-5);
  // The absolute level: DC x Sfx x the centre pan of a mono source (equal power: 1/sqrt 2).
  SND_CHECK_NEAR(tone, kDc * DbRatio(-1.0) * std::sqrt(0.5), 1e-5);
  SND_CHECK_EQ(en.e->StopAll(), 2u);
  SND_CHECK_EQ(en.e->StopAll(), 0u);  // nothing left
  en.Frames(4);
  SND_CHECK_EQ(en.L(), 0.0f);
  SND_CHECK_EQ(en.e->Status().activePlaybacks, 0u);
}

// D2: cone and a turned listener against the model, and following a moving emitter.
SND_TEST(IndepS7LivePreview, ConeAndListenerYawFollowTheModel) {
  auto cone = [](ProjectDocument& d) {
    d.Set(SpotG(), "cone", Json("{\"innerDeg\": 60, \"outerDeg\": 120, \"outerDb\": -12}"));
  };
  Engine en(false, cone);
  SND_REQUIRE(en.e != nullptr);
  SpatialPreview sp(en.doc, SpotG());
  SND_REQUIRE(sp.valid());
  sp.SetListenerYaw(0.5f);
  sp.MoveEmitter(1.0f, 3.0f);
  sp.SetEmitterYaw(3.0f);  // facing roughly back to the listener
  AuditionRouter router;
  router.Bind(nullptr, en.e.get());
  router.SetTarget(AuditionTarget::Preview);
  const AuditionResult a = router.Start3D(sp.Audition());
  SND_REQUIRE(a.ok);
  SND_REQUIRE(router.Update3D(sp.listener(), sp.emitter()).ok);
  en.Frames(8);
  SpatialResult want = sp.Compute(2, 1);
  SND_CHECK_NEAR(en.L(), kDc * want.outputGain[0], 2e-5);
  SND_CHECK_NEAR(en.R(), kDc * want.outputGain[1], 2e-5);
  // Turn the emitter away: the outer cone (-12 dB) applies.
  sp.SetEmitterYaw(0.0f);
  SND_REQUIRE(router.Update3D(sp.listener(), sp.emitter()).ok);
  en.Frames(8);
  want = sp.Compute(2, 1);
  SND_CHECK(want.coneDb < -1.0f);
  SND_CHECK_NEAR(en.L(), kDc * want.outputGain[0], 2e-5);
  SND_CHECK_NEAR(en.R(), kDc * want.outputGain[1], 2e-5);
  // A path of positions: each block follows the model of its position.
  for (int i = 0; i < 6; ++i) {
    sp.MoveEmitter(-4.0f + 1.5f * static_cast<float>(i), 2.0f + 0.5f * static_cast<float>(i));
    SND_REQUIRE(router.Update3D(sp.listener(), sp.emitter()).ok);
    en.Frames(6);
    want = sp.Compute(2, 1);
    SND_CHECK_NEAR(en.L(), kDc * want.outputGain[0], 2e-5);
    SND_CHECK_NEAR(en.R(), kDc * want.outputGain[1], 2e-5);
  }
  SND_CHECK(router.StopAll().ok);
  en.Frames(3);
  SND_CHECK_EQ(en.L(), 0.0f);
}

// D4: transitions requested inside bars switch at the sample MusicEditor::Preview computes
// (requests on or just before a bar line, and at the start: Defect_* tests).
SND_TEST(IndepS7LivePreview, TransitionSwitchesAtTheComputedSample) {
  for (const double request : {0.3, 1.7, 3.5}) {
    Engine en(true);
    SND_REQUIRE(en.e != nullptr);
    MusicEditor me(en.doc, G(236));
    SND_REQUIRE(me.valid());
    MusicSettings ms;
    ms.sampleRate = en.e->Status().sampleRate;
    ms.blockFrames = en.e->Status().blockFrames;
    const TransitionPreview p = me.Preview("Calm", "Fight", request, ms);
    SND_REQUIRE(p.valid);
    AuditionRouter router;
    router.Bind(nullptr, en.e.get());
    en.Frames(2);
    const std::uint64_t startBlock = en.e->NextBlock();
    const AuditionResult a = router.PlayTransition(TransitionAudition{snd::HashId("Tune"), snd::HashId("Calm"), snd::HashId("Fight"), request});
    SND_REQUIRE(a.ok);
    const std::uint32_t frames = ms.blockFrames;
    std::vector<float> all;
    en.e->Render(static_cast<std::uint32_t>((p.fireSeconds + 1.0) * ms.sampleRate / frames), &all);
    const std::vector<preview::PreviewTransition> tr = en.e->Transitions();
    SND_REQUIRE_MSG(!tr.empty(), "request %g", request);
    const std::uint64_t want = startBlock * frames + static_cast<std::uint64_t>(std::llround(p.fireSeconds * ms.sampleRate));
    SND_CHECK_MSG(tr.back().clock == want, "request %g: clock %llu, want %llu (fire %g s)", request,
                  static_cast<unsigned long long>(tr.back().clock), static_cast<unsigned long long>(want), p.fireSeconds);
    SND_CHECK_EQ(tr.back().to, snd::HashId("Fight"));
    const std::uint32_t ch = en.e->Status().channels;
    const std::size_t after = static_cast<std::size_t>(want - startBlock * frames + frames) * ch;
    SND_REQUIRE(after < all.size());
    const float before = all[static_cast<std::size_t>(want - startBlock * frames - 1) * ch];
    SND_CHECK_NEAR(all[after] / before, 3.0, 1e-4);
  }
}

// D6: edge values.
SND_TEST(IndepS7LivePreview, EdgeValuesAreRefusedOrHarmless) {
  Engine en;
  SND_REQUIRE(en.e != nullptr);
  std::vector<float> out;
  SND_CHECK_EQ(en.e->Render(0, &out), 0u);
  SND_CHECK(out.empty());
  SND_CHECK(!en.e->MoveEmitter(12345u, EmitterPose{}));
  SND_CHECK(!en.e->Stop(0u));
  SND_CHECK(!en.e->SetMusicState(0u, snd::HashId("Calm"), 0));
  const std::uint64_t flat = en.e->Play(snd::HashId("Tone"), nullptr);
  SND_REQUIRE(flat != 0u);
  SND_CHECK(!en.e->MoveEmitter(flat, EmitterPose{}));  // 2D: no emitter to move
  SND_CHECK(!en.e->SetMusicState(flat, 0u, 0));         // no state
  EmitterPose bad;
  bad.position = Vec3f{std::numeric_limits<float>::quiet_NaN(), 0.0f, INFINITY};
  const std::uint64_t spot = en.e->Play(snd::HashId("Spot"), &bad);
  en.Frames(4);
  SND_CHECK(Finite(en.out));
  if (spot != 0u) en.e->MoveEmitter(spot, bad);
  ListenerPose lb;
  lb.position = Vec3f{INFINITY, -INFINITY, std::numeric_limits<float>::quiet_NaN()};
  en.e->SetListener(lb);
  en.e->SetParam(snd::HashId("Knob"), std::numeric_limits<float>::quiet_NaN());
  en.e->SetParam(snd::HashId("Knob"), 1e30f);
  en.Frames(8);
  SND_CHECK(Finite(en.out));
  // Many plays (beyond the emitter slots): refused politely, the engine keeps running.
  std::uint32_t started = 0;
  EmitterPose ok;
  ok.position = Vec3f{1.0f, 0.0f, 1.0f};
  for (int i = 0; i < 200; ++i) started += en.e->Play(snd::HashId("Spot"), &ok) != 0u ? 1u : 0u;
  SND_CHECK(started >= 1u);
  SND_CHECK(started <= en.e->config().max_emitters);
  en.Frames(4);
  SND_CHECK(Finite(en.out));
  en.e->StopAll();
  en.Frames(4);
  SND_CHECK_EQ(en.e->Status().activePlaybacks, 0u);
  // The emitters came back: a 3D play starts again.
  SND_CHECK(en.e->Play(snd::HashId("Spot"), &ok) != 0u);
}

// D8: the router without a destination says why and does nothing.
SND_TEST(IndepS7LivePreview, RouterWithoutDestinationSaysWhy) {
  AuditionRouter router;
  std::string why;
  router.SetTarget(AuditionTarget::Preview);
  SND_CHECK(!router.Ready(&why));
  SND_CHECK(!why.empty());
  AuditionRequest q;
  q.eventId = snd::HashId("Tone");
  AuditionResult r = router.PlayEvent(q);
  SND_CHECK(!r.ok);
  SND_CHECK(!r.error.empty());
  router.SetTarget(AuditionTarget::Game);
  why.clear();
  SND_CHECK(!router.Ready(&why));
  SND_CHECK(!why.empty());
  r = router.PlayTransition(TransitionAudition{1, 2, 3, 0.5});
  SND_CHECK(!r.ok);
  SND_CHECK(!router.StopAll().ok || router.playing().empty());
  SND_CHECK(!router.Update3D(ListenerPose{}, EmitterPose{}).ok);
}

// D5: create and destroy repeatedly, with sounds playing, a live link client connected and the
// real-time (Silent) output (ASan: no leak, no use after free).
SND_TEST(IndepS7LivePreview, CreateAndDestroyRepeatedly) {
  ProjectDocument doc;
  Load(doc);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  for (int i = 0; i < 20; ++i) {
    preview::PreviewEngineDesc d;
    d.output = (i % 2) == 0 ? PreviewOutput::Manual : PreviewOutput::Silent;
    std::string err;
    std::unique_ptr<preview::PreviewEngine> e = preview::PreviewEngine::Create(d, &err);
    SND_REQUIRE_MSG(e != nullptr, "%s", err.c_str());
    SND_REQUIRE_MSG(e->LoadBuild(b, &err), "%s", err.c_str());
    EmitterPose p;
    p.position = Vec3f{0.0f, 0.0f, 2.0f};
    e->Play(snd::HashId("Tone"), nullptr);
    e->Play(snd::HashId("Spot"), &p);
    Connection conn;
    if ((i % 3) == 0) {
      ConnectionDesc cd;
      cd.connector = e->connector();
      cd.projectBuildHash = b.buildHash;
      conn.Open(cd);
    }
    if (d.output == PreviewOutput::Manual) {
      e->Render(4, nullptr);
    } else {
      for (int k = 0; k < 5; ++k) {
        e->Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    if ((i % 4) == 0) SND_CHECK(e->LoadBuild(b, &err));  // a second load replaces the first
    conn.Close();
    e.reset();  // with the sounds still playing
  }
}
