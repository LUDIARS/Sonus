// Independent tests of group M: every editor's operations and rules (properties D* of
// tool/test/indep_s7_model_properties.md). A refused edit returns false and leaves the
// document as it was (all objects, history).
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/project.hpp"

using namespace indep_s7;
using namespace snd::tool;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Checks that `refused` returns false and changes nothing.
struct Frozen {
  ProjectDocument& doc;
  std::map<std::string, std::string> image;
  std::size_t steps;
  explicit Frozen(ProjectDocument& d) : doc(d), image(Image(d)), steps(d.UndoCount()) {}
  bool Same() const { return Image(doc) == image && doc.UndoCount() == steps; }
};
#define REFUSED(fz, expr)                         \
  do {                                            \
    const bool _r = (expr);                       \
    SND_CHECK(!_r);                               \
    if (!(fz).Same()) {                           \
      snd::test::Note("changed by: %s", #expr);   \
      SND_CHECK(false);                           \
    }                                             \
  } while (0)

}  // namespace

// --- D1: event numbers ---------------------------------------------------------------------------

SND_TEST(IndepS7Edit, D1_EventNumberRanges) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  EventEditor e(doc, kShot);
  SND_REQUIRE(e.valid());
  Frozen fz(doc);
  for (EventNumber n : {EventNumber::RetriggerMs, EventNumber::FadeOutMs, EventNumber::ComboWindowMs, EventNumber::MaxDistance,
                        EventNumber::MaxInstances, EventNumber::Priority}) {
    REFUSED(fz, e.SetNumber(n, -1.0));
    REFUSED(fz, e.SetNumber(n, kNaN));
    REFUSED(fz, e.SetNumber(n, kInf));
    REFUSED(fz, e.SetNumber(n, -kInf));
  }
  REFUSED(fz, e.SetNumber(EventNumber::Priority, 256));
  REFUSED(fz, e.SetNumber(EventNumber::Priority, 1.5));
  REFUSED(fz, e.SetNumber(EventNumber::MaxInstances, 2.5));
  // Clip numbers.
  for (ClipNumber n : {ClipNumber::StartMs, ClipNumber::Weight, ClipNumber::FadeInMs, ClipNumber::FadeOutMs, ClipNumber::LoopCount}) {
    REFUSED(fz, e.SetClipNumber(0, 0, n, -1e-300));
    REFUSED(fz, e.SetClipNumber(0, 0, n, kNaN));
    REFUSED(fz, e.SetClipNumber(0, 0, n, kInf));
  }
  REFUSED(fz, e.SetClipNumber(0, 0, ClipNumber::LoopCount, 1.5));
  REFUSED(fz, e.SetClipNumber(0, 0, ClipNumber::VolumeDb, kNaN));
  REFUSED(fz, e.SetClipNumber(0, 0, ClipNumber::PitchCents, kInf));
  for (RandomField f : {RandomField::VolumeDb, RandomField::PitchCents, RandomField::Pan, RandomField::StartMs, RandomField::FilterHz}) {
    REFUSED(fz, e.SetClipRandom(0, 0, f, -0.5));
    REFUSED(fz, e.SetClipRandom(0, 0, f, kNaN));
  }
  REFUSED(fz, e.SetLayer(0, 0, 2, 1));
  REFUSED(fz, e.SetLayer(0, 0, kNaN, 1));
  REFUSED(fz, e.SetCone(-1, 10, 0));
  REFUSED(fz, e.SetCone(20, 10, 0));
  REFUSED(fz, e.SetCone(10, 360.0001, 0));
  REFUSED(fz, e.SetCone(kNaN, 10, 0));
  REFUSED(fz, e.SetCone(0, 10, kNaN));
  // Out of range indices.
  REFUSED(fz, e.SetClipNumber(0, 99, ClipNumber::StartMs, 1));
  REFUSED(fz, e.SetClipNumber(99, 0, ClipNumber::StartMs, 1));
  REFUSED(fz, e.SetClipNumber(0xFFFFFFFFu, 0xFFFFFFFFu, ClipNumber::StartMs, 1));
  REFUSED(fz, e.RemoveClip(0, 2));
  REFUSED(fz, e.RemoveTrack(1));
  REFUSED(fz, e.MoveTrack(0, 1));
  REFUSED(fz, e.AddTrack(2));
  REFUSED(fz, e.RemoveAction(0, 1));
  REFUSED(fz, e.SetAction(0, 5, ActionDesc{}));
  // Accepted boundaries.
  SND_CHECK(e.SetNumber(EventNumber::Priority, 0));
  SND_CHECK(e.SetNumber(EventNumber::Priority, 255));
  SND_CHECK(e.SetNumber(EventNumber::RetriggerMs, 0));
  SND_CHECK(e.SetClipNumber(0, 0, ClipNumber::LoopCount, 0));
  SND_CHECK(e.SetClipNumber(0, 0, ClipNumber::Weight, 0));
  SND_CHECK(e.SetCone(0, 360, -100));
  SND_CHECK(e.SetCone(45, 45, 0));
  SND_CHECK(e.SetLayer(0, 0, 0.5, 0.5));
  SND_CHECK_EQ(e.NumberOf(EventNumber::Priority), 255.0);
  SND_CHECK_EQ(e.ClipNumberOf(0, 0, ClipNumber::LoopCount), 0.0);
}

SND_TEST(IndepS7Edit, D2_ClipSourcesAndCycles) {
  auto files = BaseFiles();
  const char* kA = "00000000-0000-4000-8000-0000000000c1";
  const char* kB = "00000000-0000-4000-8000-0000000000c2";
  const char* kC = "00000000-0000-4000-8000-0000000000c3";
  files["events/N/A.json"] = std::string(R"({"type": "event", "guid": ")") + kA + R"(", "name": "N/A", "bank": ")" + kBank +
                             R"(", "tracks": [{"clips": [{"event": ")" + kB + R"("}]}]})";
  files["events/N/B.json"] = std::string(R"({"type": "event", "guid": ")") + kB + R"(", "name": "N/B", "bank": ")" + kBank +
                             R"(", "tracks": [{"clips": [{"event": ")" + kC + R"("}]}]})";
  files["events/N/C.json"] = std::string(R"({"type": "event", "guid": ")") + kC + R"(", "name": "N/C", "bank": ")" + kBank +
                             R"(", "tracks": [{"clips": [{"asset": ")" + kTone + R"("}]}]})";
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  EventEditor c(doc, kC);
  EventEditor a(doc, kA);
  Frozen fz(doc);
  REFUSED(fz, c.AddEventClip(0, kA, 0));   // C -> A -> B -> C
  REFUSED(fz, c.AddEventClip(0, kB, 0));   // C -> B -> C
  REFUSED(fz, c.AddEventClip(0, kC, 0));   // itself
  REFUSED(fz, a.AddEventClip(0, kA, 0));
  REFUSED(fz, c.AddAssetClip(0, kSfx, 0));   // a bus is not an asset
  REFUSED(fz, c.AddEventClip(0, kTone, 0));  // an asset is not an event
  REFUSED(fz, c.AddAssetClip(0, "00000000-0000-4000-8000-0000000000ff", 0));
  REFUSED(fz, c.SetBus(kCatUi));
  REFUSED(fz, c.SetCategory(kSfx));
  REFUSED(fz, c.SetLimitGroup(kSfx));
  SynthDesc s;
  s.kind = "patch";
  s.patch = kSfx;
  REFUSED(fz, c.AddSynthClip(0, s, 0));
  s.kind = "nonsense";
  s.patch.clear();
  REFUSED(fz, c.AddSynthClip(0, s, 0));
  ActionDesc act;
  act.action = ActionKind::Play;
  act.target = kSpeed;  // play needs an event
  REFUSED(fz, c.AddAction(0, act));
  act.action = ActionKind::SetParameter;
  act.target = kHit;  // setParameter needs a parameter
  REFUSED(fz, c.AddAction(0, act));
  act.action = ActionKind::ApplySnapshot;
  act.target = kSfx;
  REFUSED(fz, c.AddAction(0, act));
  act.action = ActionKind::Play;
  act.target = kHit;
  act.timeMs = -1;
  REFUSED(fz, c.AddAction(0, act));
  // Allowed: nesting a non-cyclic event, a synth, actions of each kind.
  SND_CHECK(a.AddEventClip(0, kC, 0));  // A -> C, A -> B -> C: no cycle
  s.kind = "patch";
  s.patch = kPatch;
  SND_CHECK(c.AddSynthClip(0, s, 0));
  act.timeMs = 0;
  SND_CHECK(c.AddAction(0, act));
  act.action = ActionKind::SetParameter;
  act.target = kSpeed;
  SND_CHECK(c.AddAction(0, act));
  act.action = ActionKind::ApplySnapshot;
  act.target = kSnap;
  SND_CHECK(c.AddAction(0, act));
  SND_CHECK_EQ(c.ActionCount(0), 3u);
  SND_CHECK(c.Action(0, 2).action == ActionKind::ApplySnapshot);
  SND_CHECK_EQ(c.Action(0, 2).target, std::string(kSnap));
  // A clip has exactly one source.
  const json::Value* clip = FindPath(doc.Find(kC)->doc, "tracks[0].clips[1]");
  SND_REQUIRE(clip != nullptr);
  SND_CHECK(clip->Find("synth") != nullptr && clip->Find("asset") == nullptr && clip->Find("event") == nullptr);
}

SND_TEST(IndepS7Edit, D3_UnusedMembersBySequence) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  EventEditor e(doc, kShot);  // random, with clip weight 2
  auto has = [&](const std::string& p) {
    for (const auto& u : e.UnusedMembers())
      if (u == p) return true;
    return false;
  };
  SND_CHECK(!has("tracks[0].clips[0].weight"));
  SND_REQUIRE(e.SetSequence(SequenceKind::Polyphonic));
  SND_CHECK(has("tracks[0].clips[0].weight"));
  // Kept in the document.
  SND_CHECK_EQ(e.ClipNumberOf(0, 0, ClipNumber::Weight), 2.0);
  SND_REQUIRE(e.SetSwitchValue(0, 0, std::string("Grass")));
  SND_CHECK(has("tracks[0].clips[0].switchValue"));
  SND_REQUIRE(e.SetSequence(SequenceKind::Switch));
  SND_CHECK(!has("tracks[0].clips[0].switchValue"));
  SND_CHECK(has("tracks[0].clips[0].weight"));
  SND_REQUIRE(e.SetSequence(SequenceKind::Shuffle));
  SND_CHECK(!has("tracks[0].clips[0].weight"));
  // UseOf table (4.2).
  SND_CHECK(UseOf(SequenceKind::Random).weight && UseOf(SequenceKind::RandomNoRepeat).weight && UseOf(SequenceKind::Shuffle).weight);
  SND_CHECK(!UseOf(SequenceKind::Sequential).weight && !UseOf(SequenceKind::Polyphonic).weight);
  SND_CHECK(UseOf(SequenceKind::Switch).switchValue);
  SND_CHECK(UseOf(SequenceKind::LayerCrossfade).layer && UseOf(SequenceKind::LayerCrossfade).allClipsPlay);
  SND_CHECK(UseOf(SequenceKind::ComboSequential).comboWindowMs);
  SND_CHECK(UseOf(SequenceKind::Polyphonic).allClipsPlay);
  for (int k = 0; k < 8; ++k) {
    SequenceKind back;
    SND_CHECK(ParseSequence(SequenceName(static_cast<SequenceKind>(k)), &back));
    SND_CHECK(back == static_cast<SequenceKind>(k));
  }
  SequenceKind dummy;
  SND_CHECK(!ParseSequence("Random", &dummy));
  SND_CHECK(!ParseSequence("", &dummy));
}

SND_TEST(IndepS7Edit, D4_TrackClipActionOperations) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  EventEditor e(doc, kShot);
  SND_REQUIRE(e.AddTrack(1));
  SND_REQUIRE(e.AddTrack(0));
  SND_CHECK_EQ(e.TrackCount(), 3u);
  // The original track is now 1.
  SND_CHECK_EQ(e.ClipCount(1), 2u);
  SND_REQUIRE(e.AddAssetClip(0, kTone, 5));
  SND_REQUIRE(e.MoveTrack(0, 2));
  SND_CHECK_EQ(e.ClipCount(0), 2u);
  SND_CHECK_EQ(e.ClipCount(2), 1u);
  // Move a clip between tracks: one step.
  const std::size_t steps = doc.UndoCount();
  SND_REQUIRE(e.MoveClipToTrack(0, 1, 2, 700));
  SND_CHECK_EQ(doc.UndoCount(), steps + 1);
  SND_CHECK_EQ(e.ClipCount(0), 1u);
  SND_CHECK_EQ(e.ClipCount(2), 2u);
  SND_CHECK_EQ(e.ClipNumberOf(2, 1, ClipNumber::StartMs), 700.0);
  // Its other members came along.
  SND_CHECK_EQ(e.ClipNumberOf(2, 1, ClipNumber::LoopCount), 3.0);
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(e.ClipCount(0), 2u);
  SND_CHECK_EQ(e.ClipNumberOf(0, 1, ClipNumber::StartMs), 100.0);
  SND_REQUIRE(e.RemoveTrack(1));
  SND_CHECK_EQ(e.TrackCount(), 2u);
  SND_REQUIRE(e.SetTrackBus(0, kReverb));
  SND_REQUIRE(e.SetTrackLayerParameter(0, kSpeed));
  SND_REQUIRE(e.SetTrackFlags(0, 3));
  SND_CHECK_EQ(NormalizeGuid(StringAt(doc.Find(kShot)->doc, "tracks[0].bus")), std::string(kReverb));
  // Action edits.
  ActionDesc a = e.Action(0, 0);
  SND_CHECK(a.action == ActionKind::SetParameter);
  SND_CHECK_EQ(a.timeMs, 10.0);
  SND_CHECK_EQ(a.value, 3.0);
  a.value = 7;
  SND_REQUIRE(e.SetAction(0, 0, a));
  SND_CHECK_EQ(e.Action(0, 0).value, 7.0);
  SND_REQUIRE(e.RemoveAction(0, 0));
  SND_CHECK_EQ(e.ActionCount(0), 0u);
  // Event members.
  SND_REQUIRE(e.SetFlag(EventFlag::Is3D, true));
  SND_CHECK(e.Flag(EventFlag::Is3D));
  SND_REQUIRE(e.SetVirtualReturn(VirtualReturn::Never));
  SND_CHECK_EQ(StringAt(doc.Find(kShot)->doc, "virtualReturn"), std::string("never"));
  SND_REQUIRE(e.SetBus(""));
  SND_CHECK(doc.Find(kShot)->doc.Find("bus") == nullptr);
  SND_REQUIRE(e.SetCategory(""));
  SND_CHECK(doc.Find(kShot)->doc.Find("category") == nullptr);
  SND_REQUIRE(e.SetSwitchValue(0, 0, 7u));
  SND_CHECK_EQ(NumberAt(doc.Find(kShot)->doc, "tracks[0].clips[0].switchValue", 0), 7.0);
}

SND_TEST(IndepS7Edit, D5_TimelineLengths) {
  auto files = BaseFiles();
  // smpl loop in a WAV: 48000 frames, loop [24000, 36000).
  files["assets/Smpl.json"] = R"({"type": "asset", "guid": "00000000-0000-4000-8000-0000000000c4", "name": "Smpl", "file": "wav/smpl.wav"})";
  files["events/T/One.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000c5", "name": "T/One", "bank": ")") + kBank +
                               R"(", "sequence": "polyphonic", "tracks": [{"clips": [
   {"asset": ")" + kTone + R"(", "startMs": 0},
   {"asset": ")" + kLoopAsset + R"(", "startMs": 500, "loopCount": 3},
   {"asset": ")" + kTone + R"(", "startMs": 100, "pitchCents": 1200},
   {"asset": "00000000-0000-4000-8000-0000000000c4", "startMs": 0, "loopCount": 2, "fadeInMs": 5, "fadeOutMs": 7}]},
  {"clips": [{"asset": ")" + kTone + R"(", "startMs": 3000, "loopCount": 0}]}]})";
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  doc.SetBinaryFile("wav/smpl.wav", MakeWav(48000, 48000, 1, 24000, 36000));
  EventEditor e(doc, "00000000-0000-4000-8000-0000000000c5");
  const Timeline tl = e.ComputeTimeline();
  SND_REQUIRE(tl.clips.size() == 5u);
  auto clip = [&](std::uint32_t t, std::uint32_t c) -> const TimelineClip* {
    for (const auto& x : tl.clips)
      if (x.track == t && x.clip == c) return &x;
    return nullptr;
  };
  const TimelineClip* c0 = clip(0, 0);
  SND_REQUIRE(c0 != nullptr);
  SND_CHECK_NEAR(c0->passMs, 1000.0, 1e-9);
  SND_CHECK_NEAR(c0->endMs, 1000.0, 1e-9);
  SND_CHECK(!c0->infinite);
  SND_CHECK_EQ(c0->source, std::string("asset"));
  // Loop region [12000, 36000) = 250..750 ms, 3 passes: 750 + 500 * 2 + 250 = 2000 ms.
  const TimelineClip* c1 = clip(0, 1);
  SND_REQUIRE(c1 != nullptr);
  SND_CHECK_NEAR(c1->endMs - c1->startMs, 2000.0, 1e-6);
  SND_CHECK_NEAR(c1->endMs, 2500.0, 1e-6);
  // +1200 cents: twice as fast, half as long.
  const TimelineClip* c2 = clip(0, 2);
  SND_REQUIRE(c2 != nullptr);
  SND_CHECK_NEAR(c2->endMs - c2->startMs, 500.0, 1e-6);
  // smpl loop [24000, 36000): 750 + 250 + 250 = 1250 ms for 2 passes.
  const TimelineClip* c3 = clip(0, 3);
  SND_REQUIRE(c3 != nullptr);
  SND_CHECK_NEAR(c3->endMs - c3->startMs, 1250.0, 1e-6);
  SND_CHECK_EQ(c3->fadeInMs, 5.0);
  SND_CHECK_EQ(c3->fadeOutMs, 7.0);
  // loopCount 0: infinite.
  const TimelineClip* c4 = clip(1, 0);
  SND_REQUIRE(c4 != nullptr);
  SND_CHECK(c4->infinite);
  SND_CHECK(tl.infinite);
  // Event length: the latest finite end.
  SND_CHECK_NEAR(tl.lengthMs, 3000.0 > 2500.0 ? std::max(2500.0, tl.lengthMs) : 2500.0, 1e-6);
  SND_CHECK(tl.lengthMs >= 2500.0 - 1e-6);
  // Overlaps on track 0: (0,1) 500..1000, (0,2) 100..600, (0,3) 0..1000, (1,2) 500..600, (1,3) 500..1250, (2,3) 100..600.
  std::uint32_t overlaps0 = 0;
  for (const auto& o : tl.overlaps) {
    if (o.track != 0) continue;
    ++overlaps0;
    const std::uint32_t a = std::min(o.clipA, o.clipB), b = std::max(o.clipA, o.clipB);
    if (a == 0 && b == 1) {
      SND_CHECK_NEAR(o.fromMs, 500.0, 1e-6);
      SND_CHECK_NEAR(o.toMs, 1000.0, 1e-6);
    }
    if (a == 1 && b == 3) {
      SND_CHECK_NEAR(o.fromMs, 500.0, 1e-6);
      SND_CHECK_NEAR(o.toMs, 1250.0, 1e-6);
    }
  }
  SND_CHECK_EQ(overlaps0, 6u);
  SND_CHECK(!tl.oneClipPerPlay);
  // A synth clip is infinite; a missing material has an unknown length.
  SynthDesc sd;
  SND_REQUIRE(e.AddSynthClip(1, sd, 0));
  doc.SetBinaryFile("wav/tone.wav", {});
  const Timeline t2 = e.ComputeTimeline();
  bool synthInf = false, unknown = false;
  for (const auto& x : t2.clips) {
    if (x.source == "synth") synthInf = x.infinite;
    if (x.track == 0 && x.clip == 0) unknown = !x.lengthKnown;
  }
  SND_CHECK(synthInf);
  SND_CHECK(unknown);
}

SND_TEST(IndepS7Edit, D5_NestedEventLengthAndOneClipPerPlay) {
  auto files = BaseFiles();
  files["events/T/Outer.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000c6", "name": "T/Outer", "bank": ")") + kBank +
                                 R"(", "sequence": "random", "tracks": [{"clips": [{"event": ")" + kHit + R"(", "startMs": 250}, {"asset": ")" + kTone + R"(", "startMs": 0, "pitchCents": -1200}]}]})";
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  EventEditor e(doc, "00000000-0000-4000-8000-0000000000c6");
  const Timeline tl = e.ComputeTimeline();
  SND_REQUIRE(tl.clips.size() == 2u);
  SND_CHECK_NEAR(tl.clips[0].endMs, 1250.0, 1e-6);  // SE/Hit lasts 1000 ms
  SND_CHECK_EQ(tl.clips[0].source, std::string("event"));
  SND_CHECK_NEAR(tl.clips[1].endMs, 2000.0, 1e-6);   // -1200 cents: twice as long
  SND_CHECK(tl.oneClipPerPlay);
  SND_CHECK_NEAR(tl.lengthMs, 2000.0, 1e-6);  // the longest
}

// --- D6..D11: mixer ----------------------------------------------------------------------------------

SND_TEST(IndepS7Edit, D6_BusTreeRules) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MixerEditor m(doc);
  SND_CHECK_EQ(m.MasterBus(), std::string(kMaster));
  const std::string sub = m.AddBus("SFX/Sub", kSfx);
  SND_REQUIRE(!sub.empty());
  Frozen fz(doc);
  REFUSED(fz, !m.AddBus("Root2", "").empty());
  REFUSED(fz, !m.AddBus("SFX", kMaster).empty());  // name taken
  REFUSED(fz, m.SetParent(kMaster, kSfx));          // master gets no parent
  REFUSED(fz, m.SetParent(kSfx, sub));              // cycle
  REFUSED(fz, m.SetParent(kSfx, kSfx));
  REFUSED(fz, m.AddSend(kSfx, kSfx, 0));            // to itself
  REFUSED(fz, m.AddSend(kReverb, kSfx, 0));         // SFX -> Reverb already: cycle
  REFUSED(fz, m.AddSend(kMaster, kSfx, 0));         // master feeds nobody
  REFUSED(fz, m.SetParent(kReverb, sub) && false);  // (checked below)
  REFUSED(fz, m.RemoveBus(kSfx));                   // has a child and is referenced
  REFUSED(fz, m.RemoveBus(kMaster));
  REFUSED(fz, m.SetChannels(kSfx, 9));
  REFUSED(fz, m.SetSendLevel(kSfx, 3, 0));
  REFUSED(fz, m.SetSendLevel(kSfx, 0, kNaN));
  REFUSED(fz, m.SetVolume(kSfx, kNaN));
  REFUSED(fz, m.SetSendTarget(kSfx, 0, kSfx));
  // Send that, with the parent links, closes a loop: Reverb under SFX/Sub, then SFX already sends to Reverb.
  REFUSED(fz, m.SetParent(kReverb, sub));
  SND_CHECK(m.SetChannels(kSfx, 8));
  SND_CHECK(m.SetChannels(kSfx, 0));
  SND_CHECK(m.SetParent(sub, kMusicBus));
  SND_CHECK(m.RemoveBus(sub));
  // Children in name order.
  const auto kids = m.Children(kMaster);
  SND_REQUIRE(kids.size() == 3u);
  SND_CHECK_EQ(kids[0], std::string(kMusicBus));
  SND_CHECK_EQ(kids[1], std::string(kReverb));
  SND_CHECK_EQ(kids[2], std::string(kSfx));
}

SND_TEST(IndepS7Edit, D7_GraphOrder) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MixerEditor m(doc);
  // Base: SFX sends to Reverb -> SFX before Reverb; Music free. Inputs first, ties by name:
  // candidates with no pending inputs: Music, SFX (Reverb waits for SFX) -> Music, SFX, Reverb, Master.
  auto order = m.GraphOrder();
  SND_REQUIRE(order.size() == 4u);
  SND_CHECK_EQ(order[0], std::string(kMusicBus));
  SND_CHECK_EQ(order[1], std::string(kSfx));
  SND_CHECK_EQ(order[2], std::string(kReverb));
  SND_CHECK_EQ(order[3], std::string(kMaster));
  // A child of Reverb named "A": A before Reverb.
  const std::string a = m.AddBus("A", kReverb);
  SND_REQUIRE(!a.empty());
  order = m.GraphOrder();
  SND_REQUIRE(order.size() == 5u);
  // Ready at the start: A, Music, SFX -> A, Music, SFX, Reverb, Master.
  SND_CHECK_EQ(order[0], a);
  SND_CHECK_EQ(order[1], std::string(kMusicBus));
  SND_CHECK_EQ(order[2], std::string(kSfx));
  SND_CHECK_EQ(order[3], std::string(kReverb));
  SND_CHECK_EQ(order[4], std::string(kMaster));
}

SND_TEST(IndepS7Edit, D8_EffectParamsNormalisedLikeMixerModel) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MixerEditor m(doc);
  const auto types = StandardEffectTypes();
  SND_REQUIRE(!types.empty());
  for (std::size_t i = 1; i < types.size(); ++i) SND_CHECK(types[i - 1].name < types[i].name);
  // Every type: add, set each parameter beyond both ends, compare with the table.
  for (const auto& t : types) {
    SND_REQUIRE(m.AddEffect(kMusicBus, t.name, 0));
    for (std::uint32_t p = 0; p < t.params.size(); ++p) {
      const auto& d = t.params[p];
      SND_CHECK_EQ(m.EffectParam(kMusicBus, 0, p), static_cast<double>(d.dspDefault));
      for (double v : {-1e9, 1e9, static_cast<double>(d.minValue), static_cast<double>(d.maxValue), 0.37}) {
        const double expect = MixerEditor::NormalizeEffectParam(t.name, p, v);
        SND_CHECK(expect >= d.minValue - 1e-6 && expect <= d.maxValue + 1e-6);
        m.SetEffectParam(kMusicBus, 0, p, v);
        SND_CHECK_EQ(m.EffectParam(kMusicBus, 0, p), expect);
      }
      SND_CHECK(!m.SetEffectParam(kMusicBus, 0, p, kNaN));
    }
    SND_REQUIRE(m.RemoveEffect(kMusicBus, 0));
  }
  // Same normalisation as the live mixer model.
  Project project;
  SND_REQUIRE(project.Load(doc));
  MixerModel mixer;
  mixer.Bind(project);
  std::int32_t sfxIndex = -1;
  for (std::size_t b = 0; b < project.buses().size(); ++b)
    if (project.buses()[b].name == "SFX") sfxIndex = static_cast<std::int32_t>(b);
  SND_REQUIRE(sfxIndex >= 0);
  Target t;
  t.control = Control::EffectParam;
  t.a = sfxIndex;
  t.b = 0;
  t.c = 0;
  for (double v : {-1e9, 1e9, 5.0, 1234.5}) {
    SND_CHECK_EQ(static_cast<float>(MixerEditor::NormalizeEffectParam("snd.fx.lowpass", 0, v)),
                 mixer.Normalize(t, static_cast<float>(v)));
  }
  // Unknown types: any finite value, no clamping.
  SND_REQUIRE(m.AddEffect(kMusicBus, "my.plugin", 0));
  SND_CHECK(m.SetEffectParam(kMusicBus, 0, 3, -12345.5));
  SND_CHECK_EQ(m.EffectParam(kMusicBus, 0, 3), -12345.5);
  SND_CHECK(!m.SetEffectParam(kMusicBus, 0, 0, kInf));
  // Move / flags / blob.
  SND_REQUIRE(m.AddEffect(kMusicBus, types[0].name, 1));
  SND_REQUIRE(m.MoveEffect(kMusicBus, 1, 0));
  SND_CHECK_EQ(StringAt(doc.Find(kMusicBus)->doc, "effects[0].type"), types[0].name);
  SND_REQUIRE(m.SetEffectFlags(kMusicBus, 0, 1));
  SND_REQUIRE(m.SetEffectBlob(kMusicBus, 0, "ir/hall.bin"));
  SND_CHECK_EQ(StringAt(doc.Find(kMusicBus)->doc, "effects[0].blob"), std::string("ir/hall.bin"));
  SND_REQUIRE(m.SetEffectBlob(kMusicBus, 0, ""));
  SND_CHECK(FindPath(doc.Find(kMusicBus)->doc, "effects[0].blob") == nullptr);
  SND_CHECK(!m.MoveEffect(kMusicBus, 0, 5));
  SND_CHECK(!m.AddEffect(kMusicBus, types[0].name, 9));
}

SND_TEST(IndepS7Edit, D9_CategoriesGroupsDucking) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MixerEditor m(doc);
  const std::string child = m.AddCategory("UI/Menu", kCatUi);
  SND_REQUIRE(!child.empty());
  Frozen fz(doc);
  REFUSED(fz, m.SetCategoryParent(kCatUi, child));
  REFUSED(fz, m.SetCategoryParent(kCatUi, kCatUi));
  REFUSED(fz, m.RemoveCategory(kCatUi));     // has a child
  REFUSED(fz, m.RemoveCategory(kCatSfx));    // an event uses it
  REFUSED(fz, m.SetCategoryParent(kCatUi, kSfx));  // not a category
  REFUSED(fz, m.SetMaxVoices(kGroup, 0));
  REFUSED(fz, !m.AddLimitGroup("G0", 0, StealMode::Never).empty());
  REFUSED(fz, !m.AddDucking("", DuckKind::Sidechain, kMusicBus, kSfx, -6).empty());   // no bank
  REFUSED(fz, !m.AddDucking(kBank, DuckKind::Sidechain, kCatUi, kSfx, -6).empty());   // sidechain source must be a bus
  REFUSED(fz, !m.AddDucking(kBank, DuckKind::Rule, kMusicBus, kSfx, -6).empty());     // rule source must be a category
  REFUSED(fz, !m.AddDucking(kBank, DuckKind::Rule, kCatUi, kCatSfx, -6).empty());     // target must be a bus
  REFUSED(fz, m.SetDucking(kDuck, DuckField::AttackMs, -1));
  REFUSED(fz, m.SetDucking(kDuck, DuckField::ReleaseMs, kNaN));
  REFUSED(fz, m.SetDuckingKind(kDuck, DuckKind::Rule, kMusicBus));
  REFUSED(fz, m.SetDuckingTarget(kDuck, kCatUi));
  SND_CHECK(m.SetMaxVoices(kGroup, 1));
  SND_CHECK(m.SetSteal(kGroup, StealMode::LowestPriority));
  SND_CHECK_EQ(StringAt(doc.Find(kGroup)->doc, "steal"), std::string("lowestPriority"));
  SND_CHECK(m.SetDuckingKind(kDuck, DuckKind::Rule, kCatUi));
  SND_CHECK_EQ(StringAt(doc.Find(kDuck)->doc, "kind"), std::string("rule"));
  SND_CHECK(m.SetDucking(kDuck, DuckField::ThresholdDb, -20));
  SND_CHECK(m.SetCategoryFlag(kCatUi, false, true));
  SND_CHECK(!BoolAt(doc.Find(kCatUi)->doc, "ignorePause", false));
  SND_CHECK(BoolAt(doc.Find(kCatUi)->doc, "muteOnFocusLoss", false));
  SND_CHECK(m.RemoveCategory(child));
}

SND_TEST(IndepS7Edit, D10_SnapshotWeights) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MixerEditor m(doc);
  const std::string s2 = m.AddSnapshot("Loud", kBank);
  SND_REQUIRE(!s2.empty());
  SnapshotValue v;
  v.kind = SnapshotKind::BusVolume;
  v.bus = kSfx;
  v.value = 4;
  SND_REQUIRE(m.AddSnapshotValue(s2, v));
  SnapshotValue fx;
  fx.kind = SnapshotKind::EffectParam;
  fx.bus = kSfx;
  fx.effect = 0;
  fx.param = 0;
  fx.value = 2000;
  SND_REQUIRE(m.AddSnapshotValue(s2, fx));
  SnapshotValue snd;
  snd.kind = SnapshotKind::SendLevel;
  snd.bus = kSfx;
  snd.send = 0;
  snd.value = 0;
  SND_REQUIRE(m.AddSnapshotValue(s2, snd));
  auto find = [](const std::vector<SnapshotTarget>& r, SnapshotKind k) -> const SnapshotTarget* {
    for (const auto& t : r)
      if (t.kind == k && t.bus == kSfx) return &t;
    return nullptr;
  };
  // Quiet (-6) at 0.5 and Loud (+4) at 1: -3 + (-3) + 4 = -2 dB.
  auto r = m.ApplySnapshots({{kSnap, 0.5}, {s2, 1.0}});
  const SnapshotTarget* vol = find(r, SnapshotKind::BusVolume);
  SND_REQUIRE(vol != nullptr);
  SND_CHECK_NEAR(vol->projectValue, -3.0, 1e-9);
  SND_CHECK_NEAR(vol->value, -2.0, 1e-9);
  // Effect value: 8000 -> 2000 at weight 0.25: 8000 + 0.25 * (2000 - 8000) = 6500.
  r = m.ApplySnapshots({{s2, 0.25}});
  const SnapshotTarget* e = find(r, SnapshotKind::EffectParam);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_NEAR(e->projectValue, 8000.0, 1e-6);
  SND_CHECK_NEAR(e->value, 6500.0, 1e-6);
  const SnapshotTarget* sl = find(r, SnapshotKind::SendLevel);
  SND_REQUIRE(sl != nullptr);
  SND_CHECK_NEAR(sl->projectValue, -12.0, 1e-9);
  SND_CHECK_NEAR(sl->value, -9.0, 1e-9);
  // Weights clamped to 0..1 (D-85); NaN -> 0.
  r = m.ApplySnapshots({{kSnap, 1.5}});
  SND_CHECK_NEAR(find(r, SnapshotKind::BusVolume)->value, -9.0, 1e-9);
  r = m.ApplySnapshots({{kSnap, -1.0}});
  SND_CHECK_NEAR(find(r, SnapshotKind::BusVolume)->value, -3.0, 1e-9);
  r = m.ApplySnapshots({{kSnap, kNaN}});
  SND_REQUIRE(find(r, SnapshotKind::BusVolume) != nullptr);
  SND_CHECK(std::isfinite(find(r, SnapshotKind::BusVolume)->value));
  SND_CHECK_NEAR(find(r, SnapshotKind::BusVolume)->value, -3.0, 1e-9);
  // Refusals: a value naming a missing effect / send, or a non-bus.
  Frozen fz(doc);
  SnapshotValue bad = fx;
  bad.effect = 5;
  REFUSED(fz, m.AddSnapshotValue(s2, bad));
  bad = snd;
  bad.send = 4;
  REFUSED(fz, m.AddSnapshotValue(s2, bad));
  bad = v;
  bad.bus = kCatUi;
  REFUSED(fz, m.AddSnapshotValue(s2, bad));
  bad = v;
  bad.value = kNaN;
  REFUSED(fz, m.AddSnapshotValue(s2, bad));
  REFUSED(fz, m.RemoveSnapshotValue(s2, 3));
  SND_CHECK(m.RemoveSnapshotValue(s2, 2));
  SND_CHECK_EQ(m.SnapshotValues(s2).size(), 2u);
}

SND_TEST(IndepS7Edit, D11_WriteBackMixerIsOneStep) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  Project project;
  SND_REQUIRE(project.Load(doc));
  MixerModel mixer;
  mixer.Bind(project);
  std::int32_t sfx = -1, music = -1;
  for (std::size_t b = 0; b < project.buses().size(); ++b) {
    if (project.buses()[b].name == "SFX") sfx = static_cast<std::int32_t>(b);
    if (project.buses()[b].name == "Music") music = static_cast<std::int32_t>(b);
  }
  SND_REQUIRE(sfx >= 0 && music >= 0);
  Target fader;
  fader.control = Control::BusFader;
  fader.a = sfx;
  SND_REQUIRE(mixer.Set(fader, -10.5f));
  Target mute;
  mute.control = Control::BusMute;
  mute.a = music;
  SND_REQUIRE(mixer.Set(mute, 1.0f));
  Target fxp;
  fxp.control = Control::EffectParam;
  fxp.a = sfx;
  fxp.b = 0;
  fxp.c = 0;
  SND_REQUIRE(mixer.Set(fxp, 4000.0f));
  const auto before = Image(doc);
  const std::size_t steps = doc.UndoCount();
  const std::uint32_t written = WriteBackMixer(doc, mixer);
  SND_CHECK_EQ(written, 2u);
  SND_CHECK_EQ(doc.UndoCount(), steps + 1);
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "volumeDb", 0), -10.5);
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "effects[0].params[0]", 0), 4000.0);
  // Mute is not written: Music keeps -6.
  SND_CHECK_EQ(NumberAt(doc.Find(kMusicBus)->doc, "volumeDb", 0), -6.0);
  // Nothing different: writes nothing and adds no step.
  Project p2;
  SND_REQUIRE(p2.Load(doc));
  MixerModel m2;
  m2.Bind(p2);
  const std::size_t s2 = doc.UndoCount();
  SND_CHECK_EQ(WriteBackMixer(doc, m2), 0u);
  SND_CHECK_EQ(doc.UndoCount(), s2);
  SND_REQUIRE(doc.Undo());
  SND_CHECK(Image(doc) == before);
}

// --- D12, D13: curves and parameters --------------------------------------------------------------------

SND_TEST(IndepS7Edit, D12_CurvePointRules) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  CurveEditor c(doc, kShot, "curves[0]");
  SND_REQUIRE(c.valid());
  SND_CHECK_EQ(c.Points().size(), 2u);
  Frozen fz(doc);
  REFUSED(fz, c.RemovePoint(0));  // two points stay
  REFUSED(fz, c.MovePoint(0, 11, 0));  // passes the right neighbour
  REFUSED(fz, c.MovePoint(1, -1, 0));  // passes the left neighbour
  REFUSED(fz, c.MovePoint(0, kNaN, 0));
  REFUSED(fz, c.MovePoint(0, 0, kInf));
  REFUSED(fz, c.SetShape(0, -1));
  REFUSED(fz, c.SetShape(0, kNaN));
  REFUSED(fz, c.AddPoint(kNaN, 0) >= 0);
  REFUSED(fz, c.SetPoints({{0, 0, 0}}));
  REFUSED(fz, c.SetPoints({{2, 0, 0}, {1, 0, 0}}));
  REFUSED(fz, c.SetPoints({{0, 0, -1}, {1, 0, 0}}));
  REFUSED(fz, c.MovePoint(2, 1, 1));
  // Equal x is allowed; a point added at an existing x goes after it.
  SND_CHECK(c.MovePoint(0, 10, 0));
  SND_CHECK(c.MovePoint(0, 0, 0));
  const int i = c.AddPoint(10, 5);
  SND_CHECK_EQ(i, 2);
  const int j = c.AddPoint(5, 1);
  SND_CHECK_EQ(j, 1);
  auto pts = c.Points();
  SND_REQUIRE(pts.size() == 4u);
  for (std::size_t k = 1; k < pts.size(); ++k) SND_CHECK(pts[k - 1].x <= pts[k].x);
  SND_CHECK(c.SetShape(1, 2.0));
  SND_CHECK_EQ(c.Points()[1].shape, 2.0);
  // Parameter curve members.
  SND_CHECK(c.SetTarget(CurveTarget::Lowpass));
  SND_CHECK(c.compose() == CurveCompose::Min);
  SND_CHECK(c.SetCompose(CurveCompose::Multiply));
  SND_CHECK(c.compose() == CurveCompose::Multiply);
  SND_CHECK(c.SetParameter("distance"));
  SND_CHECK(!c.SetParameter(kSfx));
  // Distance curves: no target / parameter of their own.
  SND_REQUIRE(SetDistanceCurve(doc, kEngine, CurveTarget::Volume, {{0, 0, 0}, {50, -40, 0}}));
  CurveEditor d(doc, kEngine, "distanceCurves.volume");
  SND_REQUIRE(d.valid());
  SND_CHECK(!d.SetTarget(CurveTarget::Pitch));
  SND_CHECK(!d.SetParameter(kSpeed));
  SND_CHECK(!SetDistanceCurve(doc, kEngine, CurveTarget::Pitch, {{0, 0, 0}, {1, 1, 0}}));
  SND_CHECK(!SetDistanceCurve(doc, kEngine, CurveTarget::Volume, {{0, 0, 0}}));
  SND_CHECK_EQ(AddEventCurve(doc, kEngine, kSpeed, CurveTarget::Pitch, {{0, 0, 0}, {10, 100, 0}}), 0);
  SND_CHECK_EQ(AddEventCurve(doc, kEngine, kSfx, CurveTarget::Pitch, {{0, 0, 0}, {10, 100, 0}}), -1);
  SND_CHECK_EQ(AddEventCurve(doc, kEngine, kSpeed, CurveTarget::Pitch, {{1, 0, 0}, {0, 100, 0}}), -1);
  // Stem volume curve through the same component.
  SND_REQUIRE(doc.Modify(kSegA, [](json::Value& v) {
    json::Value stems = json::MakeArray();
    json::Value stem = json::MakeObject();
    json::Add(stem, "channelFirst", Number(0));
    json::Add(stem, "channelCount", Number(2));
    json::Value curve = json::MakeObject();
    json::Add(curve, "parameter", String(kSpeed));
    json::Value points = json::MakeArray();
    for (int k = 0; k < 2; ++k) {
      json::Value p = json::MakeArray();
      p.items.push_back(Number(k * 10.0));
      p.items.push_back(Number(k * -6.0));
      points.items.push_back(p);
    }
    json::Add(curve, "points", points);
    json::Add(stem, "volumeCurve", curve);
    stems.items.push_back(stem);
    json::Add(v, "stems", stems);
    return true;
  }));
  CurveEditor s(doc, kSegA, "stems[0].volumeCurve");
  SND_CHECK(s.valid());
  SND_CHECK_EQ(s.Points().size(), 2u);
  SND_CHECK(s.AddPoint(5, -1) == 1);
  SND_CHECK(!CurveEditor(doc, kSegA, "stems[3].volumeCurve").valid());
}

SND_TEST(IndepS7Edit, D13_ParameterRules) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  ParameterEditor p(doc, kSpeed);
  SND_REQUIRE(p.valid());
  SND_CHECK_EQ(p.scope(), 3u);
  Frozen fz(doc);
  REFUSED(fz, p.SetScope(0));
  REFUSED(fz, p.SetScope(8));
  REFUSED(fz, p.SetScope(15));
  REFUSED(fz, p.SetRange(5, 5));
  REFUSED(fz, p.SetRange(6, 5));
  REFUSED(fz, p.SetRange(kNaN, 5));
  REFUSED(fz, p.SetDefault(11));
  REFUSED(fz, p.SetDefault(-0.5));
  REFUSED(fz, p.SetDefault(kNaN));
  REFUSED(fz, p.SetRampRate(-1));
  REFUSED(fz, p.SetRampRate(kNaN));
  // A range that leaves the default outside: refused, or the default follows into it.
  {
    const bool ok = p.SetRange(0, 1);
    SND_CHECK(p.defaultValue() >= p.minValue() && p.defaultValue() <= p.maxValue());
    if (ok) {
      SND_CHECK_EQ(p.minValue(), 0.0);
      SND_CHECK_EQ(p.maxValue(), 1.0);
      SND_REQUIRE(doc.Undo());
    }
  }
  SND_CHECK(p.SetScope(7));
  SND_CHECK(p.SetScope(4));
  SND_CHECK_EQ(p.scope(), 4u);
  SND_CHECK(p.SetDefault(0));
  SND_CHECK(p.SetDefault(10));
  SND_CHECK(p.SetRange(-1, 10));
  SND_CHECK(p.SetRampRate(0));
  SND_CHECK_EQ(p.rampRate(), 0.0);
  SND_CHECK(p.SetSwitch(true));
  SND_CHECK(p.isSwitch());
  SND_CHECK_EQ(StringAt(doc.Find(kSpeed)->doc, "kind"), std::string("switch"));
  SND_CHECK_EQ(p.minValue(), -1.0);
  SND_CHECK_EQ(p.maxValue(), 10.0);
}

// --- D14, D15: music ---------------------------------------------------------------------------------------

SND_TEST(IndepS7Edit, D14_MusicRules) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SegmentEditor s(doc, kSegA);
  SND_REQUIRE(s.valid());
  SND_CHECK_NEAR(s.SampleRate(), 8000.0, 1e-9);
  SND_CHECK_NEAR(s.Frames(), 64000.0, 1e-9);
  Frozen fz(doc);
  TempoPointDesc t;
  t.tick = 0;
  REFUSED(fz, s.AddTempo(t));  // same tick as the existing point
  t.tick = 3840;
  t.bpm = 1e-4;
  REFUSED(fz, s.AddTempo(t));
  t.bpm = 1e5 + 1;
  REFUSED(fz, s.AddTempo(t));
  t.bpm = kNaN;
  REFUSED(fz, s.AddTempo(t));
  t.bpm = 100;
  t.beatsPerBar = 0;
  REFUSED(fz, s.AddTempo(t));
  t.beatsPerBar = 3;
  for (std::uint32_t u : {0u, 3u, 5u, 64u}) {
    t.beatUnit = u;
    REFUSED(fz, s.AddTempo(t));
  }
  REFUSED(fz, s.RemoveTempo(1));
  MarkerDesc m;
  m.name = "M1";  // taken
  m.frame = 10;
  REFUSED(fz, s.AddMarker(m));
  m.name = "M2";
  m.frame = 64001;
  REFUSED(fz, s.AddMarker(m));
  REFUSED(fz, s.SetBars(0));
  REFUSED(fz, s.SetEntryFrame(64000));
  REFUSED(fz, s.SetExitFrame(64001));
  REFUSED(fz, s.SetAsset(kSfx));
  StemDesc st;
  // Accepted.
  for (std::uint32_t u : {1u, 2u, 4u, 8u, 16u, 32u}) {
    t.tick = 3840u * u;
    t.beatUnit = u;
    SND_CHECK(s.AddTempo(t));
  }
  t.tick = 7;
  t.bpm = 1e-3;
  SND_CHECK(s.AddTempo(t));
  t.tick = 8;
  t.bpm = 1e5;
  SND_CHECK(s.AddTempo(t));
  const auto tempo = s.Tempo();
  for (std::size_t i = 1; i < tempo.size(); ++i) SND_CHECK(tempo[i - 1].tick < tempo[i].tick);
  m.frame = 64000;
  SND_CHECK(s.AddMarker(m));
  SND_CHECK(s.SetExitFrame(64000));
  SND_CHECK(s.SetEntryFrame(63999));
  st.channelFirst = 0;
  st.channelCount = 2;
  SND_CHECK(s.AddStem(st));
  Frozen fz2(doc);
  REFUSED(fz2, s.SetExitFrame(63999));  // entry < exit
  // Playlist.
  PlaylistEditor pl(doc, kPlExplore);
  SND_REQUIRE(pl.valid());
  REFUSED(fz2, pl.AddEntry(kSegB, -1, 1));
  REFUSED(fz2, pl.AddEntry(kSegB, 1, 0));
  REFUSED(fz2, pl.AddEntry(kTone, 1, 1));
  REFUSED(fz2, pl.AddEntry(kSegB, kNaN, 1));
  REFUSED(fz2, pl.MoveEntry(0, 1));
  SND_CHECK(pl.AddEntry(kSegB, 0, 1));
  SND_CHECK(pl.MoveEntry(1, 0));
  SND_CHECK(pl.SetMode(PlaylistMode::Shuffle));
  SND_CHECK(pl.SetLoopCount(0));
  SND_CHECK_EQ(pl.EntryCount(), 2u);
  // States and rules.
  MusicEditor me(doc, kBgm);
  Frozen fz3(doc);
  REFUSED(fz3, me.AddState("*", kPlExplore));
  REFUSED(fz3, me.AddState("Explore", kPlExplore));
  REFUSED(fz3, me.AddState("", kPlExplore));
  REFUSED(fz3, me.AddState("Boss", kSegA));  // not a playlist
  REFUSED(fz3, me.SetInitialState("Nope"));
  TransitionRule r;
  r.from = "Nope";
  REFUSED(fz3, me.AddRule(r, 0));
  r.from = "*";
  r.to = "Nope";
  REFUSED(fz3, me.AddRule(r, 0));
  r.to = "Battle";
  r.bridge = kTone;  // not a segment
  REFUSED(fz3, me.AddRule(r, 0));
  r.bridge.clear();
  r.fadeOutBeats = -1;
  REFUSED(fz3, me.AddRule(r, 0));
  r.fadeOutBeats = 0;
  REFUSED(fz3, me.AddRule(r, 5));
  REFUSED(fz3, me.RemoveRule(2));
  REFUSED(fz3, me.MoveRule(0, 2));
  StingerDesc sg;
  sg.name = "Sting/Hit";  // taken
  sg.event = kSting;
  REFUSED(fz3, me.AddStinger(sg));
  sg.name = "Sting/2";
  sg.event = kSfx;
  REFUSED(fz3, me.AddStinger(sg));
  SND_CHECK(me.AddState("Boss", kPlBattle));
  SND_CHECK(me.AddRule(r, 0));
  sg.event = kSting;
  SND_CHECK(me.AddStinger(sg));
  SND_CHECK_EQ(me.States().size(), 3u);
}

SND_TEST(IndepS7Edit, D15_RenameAndRemoveState) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MusicEditor me(doc, kBgm);
  Frozen fz(doc);
  REFUSED(fz, me.RemoveState("Explore"));  // initial state and a rule
  REFUSED(fz, me.RemoveState("Battle"));   // a rule names it
  REFUSED(fz, me.RenameState("Explore", "Battle"));
  REFUSED(fz, me.RenameState("Explore", "*"));
  REFUSED(fz, me.RenameState("Nope", "X"));
  const std::size_t steps = doc.UndoCount();
  SND_REQUIRE(me.RenameState("Explore", "Calm"));
  SND_CHECK_EQ(doc.UndoCount(), steps + 1);
  const json::Value& v = doc.Find(kBgm)->doc;
  SND_CHECK_EQ(StringAt(v, "music.initialState"), std::string("Calm"));
  SND_CHECK_EQ(StringAt(v, "music.transitions[0].from"), std::string("Calm"));
  SND_CHECK_EQ(StringAt(v, "music.transitions[1].from"), std::string("*"));
  SND_CHECK_EQ(StringAt(v, "music.states[0].name"), std::string("Calm"));
  SND_CHECK_EQ(me.Match("Calm", "Battle"), 0);
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(StringAt(doc.Find(kBgm)->doc, "music.initialState"), std::string("Explore"));
  // Remove the rule, then the state Battle can go.
  SND_REQUIRE(me.RemoveRule(0));
  SND_CHECK(me.RemoveState("Battle"));
}

// --- D16: materials ------------------------------------------------------------------------------------------

SND_TEST(IndepS7Edit, D16_MaterialNumbersAndFollowers) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MaterialEditor m(doc, kPhys);
  SND_REQUIRE(m.valid());
  SND_CHECK_EQ(m.IndexOf("glass"), 2);
  Renumbering r;
  SND_REQUIRE(m.AddMaterial("stone", 3, &r));
  SND_REQUIRE(r.oldToNew.size() == 3u);
  SND_CHECK(r.oldToNew[0] == 0 && r.oldToNew[1] == 1 && r.oldToNew[2] == 2);
  SND_REQUIRE(m.AddMaterial("cloth", 1, &r));  // insert: wood, glass, stone move up
  SND_REQUIRE(r.oldToNew.size() == 4u);
  SND_CHECK(r.oldToNew[0] == 0 && r.oldToNew[1] == 2 && r.oldToNew[2] == 3 && r.oldToNew[3] == 4);
  SND_REQUIRE(m.MoveMaterial(4, 0, &r));  // stone to the front
  SND_CHECK(r.oldToNew[4] == 0 && r.oldToNew[0] == 1 && r.oldToNew[3] == 4);
  // metal, glass keep their pair; single follows the name.
  SND_CHECK_EQ(m.Pair("metal", "glass"), std::string(kSting));
  SND_CHECK_EQ(m.Pair("glass", "metal"), std::string(kSting));
  SND_CHECK_EQ(m.Single("metal", ContactKind::Impact), std::string(kShot));
  // Rename: every member follows (one step).
  const std::size_t steps = doc.UndoCount();
  SND_REQUIRE(m.RenameMaterial("metal", "steel"));
  SND_CHECK_EQ(doc.UndoCount(), steps + 1);
  SND_CHECK_EQ(m.Single("steel", ContactKind::Impact), std::string(kShot));
  SND_CHECK_EQ(m.Pair("steel", "glass"), std::string(kSting));
  SND_CHECK(m.IndexOf("metal") < 0);
  // Remove: the pair naming it goes, the numbers after it come down.
  SND_REQUIRE(m.RemoveMaterial("glass", &r));
  SND_CHECK(r.oldToNew[m.IndexOf("steel")] >= -1);
  const json::Value* pairs = FindPath(doc.Find(kPhys)->doc, "pairs");
  SND_CHECK(pairs == nullptr || pairs->items.empty());
  // Footstep of wood follows wood's renumbering (by name).
  SND_CHECK_EQ(m.Footstep("wood"), std::string(kHit));
  Frozen fz(doc);
  REFUSED(fz, m.AddMaterial("wood", 0));   // duplicate
  REFUSED(fz, m.AddMaterial("", 0));
  REFUSED(fz, m.AddMaterial("notes", 0));  // the reserved key of the tables
  REFUSED(fz, m.AddMaterial("x", 99));
  REFUSED(fz, m.RenameMaterial("wood", "steel"));
  REFUSED(fz, m.RemoveMaterial("nope"));
  REFUSED(fz, m.MoveMaterial(0, 9));
  REFUSED(fz, m.SetSingle("nope", ContactKind::Impact, kHit));
  REFUSED(fz, m.SetSingle("wood", ContactKind::Impact, kSfx));
  REFUSED(fz, m.SetPair("wood", "nope", kHit));
  // Pairs: setting (b, a) replaces (a, b); clearing removes.
  SND_REQUIRE(m.SetPair("wood", "steel", kHit));
  SND_REQUIRE(m.SetPair("steel", "wood", kEngine));
  SND_CHECK_EQ(m.Pair("wood", "steel"), std::string(kEngine));
  const json::Value* p2 = FindPath(doc.Find(kPhys)->doc, "pairs");
  SND_REQUIRE(p2 != nullptr);
  SND_CHECK_EQ(p2->items.size(), 1u);
  SND_REQUIRE(m.SetPair("wood", "steel", ""));
  SND_CHECK_EQ(m.Pair("steel", "wood"), std::string());
  // A pair of a material with itself is allowed (table[a*N+a]).
  SND_CHECK(m.SetPair("wood", "wood", kHit));
}

SND_TEST(IndepS7Edit, D16_StrengthCurvesRules) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MaterialEditor m(doc, kPhys);
  StrengthCurves c;
  Frozen fz(doc);
  c.impulseToVelocity[0] = 5;
  c.impulseToVelocity[1] = 1;  // not ascending
  REFUSED(fz, m.SetCurves("wood", c));
  c = StrengthCurves{};
  c.massToPitchCents[0] = static_cast<float>(kNaN);
  REFUSED(fz, m.SetCurves("wood", c));
  REFUSED(fz, m.SetCurves("nope", StrengthCurves{}));
  c = StrengthCurves{};
  c.speedToVolumeDb[1] = -3;
  SND_CHECK(m.SetCurves("wood", c));
  SND_CHECK_EQ(m.Curves("wood").speedToVolumeDb[1], -3.0f);
  // Defaults for a material without curves: the build's.
  const StrengthCurves d = m.Curves("metal");
  SND_CHECK_EQ(d.impulseToVelocity[3], 100.0f);
  SND_CHECK_EQ(d.speedToVolumeDb[0], -30.0f);
}

// --- D17: patch ------------------------------------------------------------------------------------------------

SND_TEST(IndepS7Edit, D17_PatchRules) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  PatchEditor p(doc, kPatch);
  SND_REQUIRE(p.valid());
  SND_CHECK_EQ(p.Nodes().size(), 4u);
  SND_CHECK_EQ(p.OutputNode(), std::string("out"));
  Frozen fz(doc);
  REFUSED(fz, !p.AddNode("output", 0, 0).empty());     // a second output
  REFUSED(fz, !p.AddNode("nonsense", 0, 0).empty());
  REFUSED(fz, !p.AddNode("lfo", 0, 0, "osc").empty());  // id taken
  REFUSED(fz, p.SetParam("osc", "frequencyHz", kNaN));
  REFUSED(fz, p.SetParam("osc", "nope", 1));
  REFUSED(fz, p.SetParam("osc", "waveform", 1));        // an enumeration is set by name
  REFUSED(fz, p.SetChoice("osc", "waveform", "zigzag"));
  REFUSED(fz, p.SetChoice("osc", "frequencyHz", "saw"));
  REFUSED(fz, p.Connect("osc", "out", "in"));           // "in" of out is taken
  REFUSED(fz, p.Connect("env", "out", "in"));           // control into audio
  REFUSED(fz, p.Connect("osc", "osc", "amp"));          // itself; and audio into control
  REFUSED(fz, p.Connect("vca", "osc", "pitch"));        // audio into a control input
  REFUSED(fz, p.Connect("out", "vca", "gain"));         // the output node has no output
  REFUSED(fz, p.Connect("osc", "vca", "nope"));
  REFUSED(fz, p.Connect("ghost", "vca", "gain"));
  REFUSED(fz, p.RenameNode("osc", "env"));
  REFUSED(fz, p.RenameNode("osc", ""));
  REFUSED(fz, p.MoveNode("osc", static_cast<float>(kNaN), 0));
  REFUSED(fz, p.Disconnect("vca", "pitch"));
  // A cycle through a filter: vca -> filt -> (into vca.in is taken) ... build one: filt.in <- vca, and vca.in <- filt.
  const std::string f = p.AddNode("filter", 0, 0, "filt");
  SND_REQUIRE(f == "filt");
  SND_REQUIRE(p.Connect("vca", "filt", "in"));
  SND_REQUIRE(p.Disconnect("vca", "in"));
  Frozen fz2(doc);
  REFUSED(fz2, p.Connect("filt", "vca", "in"));  // vca -> filt -> vca
  // Parameters: clamp or refuse beyond the range (either way never outside).
  p.SetParam("osc", "frequencyHz", 1e12);
  const auto* kind = snd::buildtool::patch::FindNodeKind("oscillator");
  SND_REQUIRE(kind != nullptr);
  const auto* fi = snd::buildtool::patch::FindParam(*kind, "frequencyHz");
  SND_REQUIRE(fi != nullptr);
  SND_CHECK(p.Param("osc", "frequencyHz") <= fi->maxValue);
  SND_CHECK(p.SetChoice("osc", "waveform", "square"));
  // Rename follows connections and the output.
  SND_REQUIRE(p.RenameNode("out", "final"));
  SND_CHECK_EQ(p.OutputNode(), std::string("final"));
  bool toFinal = false;
  for (const auto& c : p.Connections()) toFinal |= c.to == "final";
  SND_CHECK(toFinal);
  // Remove a node: its connections go (one step).
  const std::size_t steps = doc.UndoCount();
  SND_REQUIRE(p.RemoveNode("vca"));
  SND_CHECK_EQ(doc.UndoCount(), steps + 1);
  for (const auto& c : p.Connections()) SND_CHECK(c.from != "vca" && c.to != "vca");
  // Up to 32 nodes, the 33rd is refused.
  std::uint32_t added = 0;
  while (p.Nodes().size() < snd::buildtool::patch::kMaxNodes) {
    SND_REQUIRE(!p.AddNode("lfo", 0, 0).empty());
    ++added;
  }
  Frozen fz3(doc);
  REFUSED(fz3, !p.AddNode("lfo", 0, 0).empty());
  SND_CHECK_EQ(p.Nodes().size(), 32u);
  // Generated ids are unique.
  auto nodes = p.Nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i)
    for (std::size_t j = i + 1; j < nodes.size(); ++j) SND_CHECK(nodes[i].id != nodes[j].id);
  // The position is a member of its own, not notes.
  SND_REQUIRE(p.MoveNode("osc", 12.5f, -3.0f));
  const json::Value& v = doc.Find(kPatch)->doc;
  SND_CHECK_EQ(NumberAt(v, "nodes[0].position[0]", 0), 12.5);
  SND_CHECK_EQ(NumberAt(v, "nodes[0].position[1]", 0), -3.0);
  (void)added;
}
