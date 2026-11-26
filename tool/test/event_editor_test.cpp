// Event editor model: every edit, the rules, undo / redo, save / load, the timeline
// (analytic lengths from the WAV), unused members per sequence kind, and the build output
// (the edited value in its bank cell).
#include <cmath>

#include "doc_fixture.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

// Base project + a looped asset (9600 frames at 48 kHz, loop 2400..7200) and a nested event.
void Load(ProjectDocument& doc) {
  std::map<std::string, std::string> f = BaseFiles();
  f["assets/loop.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(40) + "\", \"name\": \"loop\", \"file\": \"audio/loop.wav\", "
                          "\"loop\": {\"start\": 2400, \"end\": 7200}}";
  f["events/Inner.json"] = "{\"type\": \"event\", \"guid\": \"" + G(41) + "\", \"name\": \"Inner\", \"bank\": \"Main\", "
                           "\"tracks\": [{\"clips\": [{\"asset\": \"tone\", \"startMs\": 50}]}]}";
  f["events/Outer.json"] = "{\"type\": \"event\", \"guid\": \"" + G(42) + "\", \"name\": \"Outer\", \"bank\": \"Main\", "
                           "\"tracks\": [{\"clips\": [{\"event\": \"Shot\"}]}]}";
  LoadBase(doc, f);
  doc.SetBinaryFile("audio/loop.wav", ToneWav(9600));
}

double Value0(const DocBuild& b, const ProjectDocument& doc, const std::string& guid, const std::string& path) {
  const std::vector<CellRef> c = LocateField(b, doc, guid, path);
  if (c.size() != 1u) return NAN;
  return c[0].type == CellType::F32 ? BitsToFloat(c[0].bits) : static_cast<double>(c[0].bits);
}

}  // namespace

SND_TEST(EventEditor, TimelineLengthsFromTheMaterial) {
  ProjectDocument doc;
  Load(doc);
  EventEditor e(doc, G(8));
  SND_REQUIRE(e.valid());
  // Shot: one clip of tone (4800 frames = 100 ms).
  Timeline tl = e.ComputeTimeline();
  SND_REQUIRE_EQ(tl.clips.size(), std::size_t{1});
  SND_CHECK_NEAR(tl.clips[0].passMs, 100.0, 1e-9);
  SND_CHECK_NEAR(tl.lengthMs, 100.0, 1e-9);
  // Start 20 ms, pitch +1200 (twice as fast), 3 passes.
  SND_REQUIRE(e.MoveClip(0, 0, 20));
  SND_REQUIRE(e.SetClipNumber(0, 0, ClipNumber::PitchCents, 1200));
  SND_REQUIRE(e.SetClipNumber(0, 0, ClipNumber::LoopCount, 3));
  tl = e.ComputeTimeline();
  SND_CHECK_NEAR(tl.clips[0].passMs, 50.0, 1e-9);
  SND_CHECK_NEAR(tl.clips[0].endMs, 20.0 + 150.0, 1e-9);
  // A loop region: intro + region * (loops - 1) + tail. loop.wav = 200 ms, region 50..150 ms.
  SND_REQUIRE(e.AddTrack(1));
  SND_REQUIRE(e.AddAssetClip(1, G(40), 0));
  SND_REQUIRE(e.SetClipNumber(1, 0, ClipNumber::LoopCount, 4));
  tl = e.ComputeTimeline();
  SND_CHECK_NEAR(tl.clips[1].endMs, 200.0 + 3 * 100.0, 1e-9);
  SND_CHECK_NEAR(tl.lengthMs, 500.0, 1e-9);
  // loopCount 0: infinite.
  SND_REQUIRE(e.SetClipNumber(1, 0, ClipNumber::LoopCount, 0));
  tl = e.ComputeTimeline();
  SND_CHECK(tl.infinite && tl.clips[1].infinite);
  SND_CHECK_NEAR(tl.lengthMs, 170.0, 1e-9);  // the finite clips
  // Nested event: Outer plays Shot (170 ms).
  EventEditor outer(doc, G(42));
  SND_REQUIRE(e.SetClipNumber(1, 0, ClipNumber::LoopCount, 1));
  const Timeline otl = outer.ComputeTimeline();
  SND_CHECK_NEAR(otl.clips[0].passMs, 200.0, 1e-9);  // Shot's longest clip is now loop.wav once
  // Overlaps on one track.
  SND_REQUIRE(e.AddAssetClip(0, G(7), 100));  // 100..200 overlaps clip 0 (20..170)
  tl = e.ComputeTimeline();
  SND_REQUIRE_EQ(tl.overlaps.size(), std::size_t{1});
  SND_CHECK_NEAR(tl.overlaps[0].fromMs, 100.0, 1e-9);
  SND_CHECK_NEAR(tl.overlaps[0].toMs, 170.0, 1e-9);
  // A synth never ends; an unreadable asset is "unknown".
  SynthDesc s;
  SND_REQUIRE(e.AddSynthClip(1, s, 10));
  tl = e.ComputeTimeline();
  SND_CHECK(tl.clips.back().source == "synth" && tl.clips.back().infinite);
  doc.SetBinaryFile("audio/loop.wav", {1, 2, 3});
  tl = e.ComputeTimeline();
  SND_CHECK(!tl.clips[2].lengthKnown);
}

SND_TEST(EventEditor, RulesAreEnforced) {
  ProjectDocument doc;
  Load(doc);
  EventEditor e(doc, G(8));
  const std::size_t steps = doc.UndoCount();
  SND_CHECK(!e.MoveClip(0, 0, -1));
  SND_CHECK(!e.MoveClip(0, 5, 10));
  SND_CHECK(!e.SetClipNumber(0, 0, ClipNumber::Weight, -1));
  SND_CHECK(!e.SetClipNumber(0, 0, ClipNumber::LoopCount, 1.5));
  SND_CHECK(!e.SetClipNumber(0, 0, ClipNumber::FadeInMs, NAN));
  SND_CHECK(!e.SetClipRandom(0, 0, RandomField::Pan, 3));
  SND_CHECK(!e.SetClipRandom(0, 0, RandomField::VolumeDb, -1));
  SND_CHECK(!e.SetLayer(0, 0, 5, 1));
  SND_CHECK(!e.SetNumber(EventNumber::Priority, 256));
  SND_CHECK(!e.SetNumber(EventNumber::Priority, 1.5));
  SND_CHECK(!e.SetNumber(EventNumber::MaxInstances, -2));
  SND_CHECK(!e.SetCone(90, 30, -6));
  SND_CHECK(!e.SetCone(0, 400, -6));
  SND_CHECK(!e.SetBus(G(5)));             // a category is not a bus
  SND_CHECK(!e.AddAssetClip(0, G(8), 0)); // an event is not an asset
  SND_CHECK(!e.AddEventClip(0, G(8), 0)); // itself
  SND_CHECK(!e.AddEventClip(0, G(42), 0));  // Outer plays Shot: cycle
  SynthDesc bad;
  bad.kind = "patch";
  bad.patch = G(1);  // a bank, not a patch
  SND_CHECK(!e.AddSynthClip(0, bad, 0));
  ActionDesc a;
  a.action = ActionKind::SetParameter;
  a.target = G(8);  // an event, not a parameter
  SND_CHECK(!e.AddAction(0, a));
  SND_CHECK_EQ(doc.UndoCount(), steps);
  SND_CHECK(doc.PendingChanges().empty());
  // Negative volume and pitch are fine.
  SND_CHECK(e.SetClipNumber(0, 0, ClipNumber::VolumeDb, -12));
  SND_CHECK(e.SetClipNumber(0, 0, ClipNumber::PitchCents, -300));
}

SND_TEST(EventEditor, EditsUndoRedoAndRoundTrip) {
  TempDir dir("event");
  std::map<std::string, std::string> files = BaseFiles();
  WriteAll(dir, files);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  doc.SetRandom(Counter{});
  const json::Value original = doc.Find(G(8))->doc;
  EventEditor e(doc, G(8));
  SND_REQUIRE(e.SetSequence(SequenceKind::Random));
  SND_REQUIRE(e.SetNumber(EventNumber::Priority, 200));
  SND_REQUIRE(e.SetNumber(EventNumber::MaxInstances, 4));
  SND_REQUIRE(e.SetNumber(EventNumber::RetriggerMs, 30));
  SND_REQUIRE(e.SetNumber(EventNumber::FadeOutMs, 120));
  SND_REQUIRE(e.SetNumber(EventNumber::MaxDistance, 80));
  SND_REQUIRE(e.SetFlag(EventFlag::IgnorePause, true));
  SND_REQUIRE(e.SetFlag(EventFlag::Is3D, false));
  SND_REQUIRE(e.SetCone(60, 120, -9));
  SND_REQUIRE(e.SetVirtualReturn(VirtualReturn::Restart));
  SND_REQUIRE(e.SetCategory(""));
  SND_CHECK(e.SetLimitGroup(""));  // absent already: no change is success (D-177b)
  SND_REQUIRE(e.AddTrack(1));
  SND_REQUIRE(e.SetTrackBus(1, G(4)));
  SND_REQUIRE(e.AddAssetClip(1, G(7), 40));
  SND_REQUIRE(e.SetClipNumber(1, 0, ClipNumber::Weight, 3));
  SND_REQUIRE(e.SetClipRandom(1, 0, RandomField::PitchCents, 100));
  SND_REQUIRE(e.SetClipRandom(1, 0, RandomField::FilterHz, 500));
  SND_REQUIRE(e.SetClipNumber(1, 0, ClipNumber::FadeInMs, 5));
  SND_REQUIRE(e.SetClipNumber(1, 0, ClipNumber::FadeOutMs, 15));
  SND_REQUIRE(e.MoveClipToTrack(0, 0, 1, 70));
  SND_CHECK_EQ(e.ClipCount(0), 0u);
  SND_CHECK_EQ(e.ClipCount(1), 2u);
  SND_CHECK_EQ(e.ClipNumberOf(1, 1, ClipNumber::StartMs), 70.0);
  SND_CHECK_EQ(e.ClipNumberOf(1, 1, ClipNumber::VolumeDb), -3.0);  // the moved clip keeps its members
  SND_REQUIRE(e.MoveTrack(1, 0));
  ActionDesc a;
  a.timeMs = 25;
  a.action = ActionKind::SetParameter;
  a.target = G(6);
  a.value = 3;
  SND_REQUIRE(e.AddAction(0, a));
  a.action = ActionKind::ApplySnapshot;
  a.target = G(9);
  a.value = 0.5;
  SND_REQUIRE(e.SetAction(0, 0, a));
  SND_CHECK(e.Action(0, 0).action == ActionKind::ApplySnapshot && e.Action(0, 0).target == G(9));
  SND_CHECK_EQ(e.ActionCount(0), 1u);
  // Unused members for random: none of weight; switchValue would be.
  SND_REQUIRE(e.SetSwitchValue(0, 0, std::string("Grass")));
  std::vector<std::string> unused = e.UnusedMembers();
  SND_CHECK(unused.size() == 1u && unused[0] == "tracks[0].clips[0].switchValue");
  SND_REQUIRE(e.SetSequence(SequenceKind::Switch));
  unused = e.UnusedMembers();
  SND_CHECK(unused.size() == 1u && unused[0] == "tracks[0].clips[0].weight");
  // The build takes it and the bank holds the values.
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  SND_CHECK_EQ(Value0(b, doc, G(8), "priority"), 200.0);
  SND_CHECK_EQ(Value0(b, doc, G(8), "sequence"), 5.0);
  SND_CHECK_EQ(Value0(b, doc, G(8), "tracks[0].clips[1].startMs"), 70.0);
  SND_CHECK_EQ(Value0(b, doc, G(8), "tracks[0].clips[0].random.filterHz"), 500.0);
  SND_CHECK_EQ(Value0(b, doc, G(8), "tracks[0].clips[0].switchValue"), static_cast<double>(snd::HashId("Grass")));
  SND_CHECK_EQ(Value0(b, doc, G(8), "tracks[0].actions[0].value"), 0.5);
  SND_CHECK_EQ(Value0(b, doc, G(8), "cone.outerDb"), -9.0);
  SND_CHECK_EQ(Value0(b, doc, G(8), "is3D"), static_cast<double>(bf::kEventIgnorePause));  // flags: only ignorePause
  // Save and load: the same object.
  SND_REQUIRE(doc.Save());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir.path().string()));
  SND_CHECK(JsonEqual(again.Find(G(8))->doc, doc.Find(G(8))->doc));
  // Undo everything: back to the original text; redo everything: the edited text.
  const std::string edited = doc.TextOf(*doc.Find(G(8)));
  std::size_t n = 0;
  while (doc.Undo()) ++n;
  SND_CHECK(JsonEqual(doc.Find(G(8))->doc, original));
  SND_CHECK_EQ(doc.PendingChanges().size(), std::size_t{1});  // differs from the saved (edited) file
  for (std::size_t i = 0; i < n; ++i) SND_CHECK(doc.Redo());
  SND_CHECK(doc.TextOf(*doc.Find(G(8))) == edited);
}

SND_TEST(EventEditor, ClipDragMergesIntoOneStep) {
  ProjectDocument doc;
  Load(doc);
  EventEditor e(doc, G(8));
  doc.BeginGroup();
  for (int i = 0; i <= 100; i += 10) SND_CHECK(e.MoveClip(0, 0, i) || i == 0);
  doc.EndGroup();
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{1});
  SND_CHECK(doc.Undo());
  SND_CHECK_EQ(e.ClipNumberOf(0, 0, ClipNumber::StartMs), 0.0);
  // Wheel on the volume with one merge key.
  for (int i = 1; i <= 5; ++i) e.SetClipNumber(0, 0, ClipNumber::VolumeDb, -3 - i, 9);
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{1});
  // Remove / undo restores the clip.
  SND_REQUIRE(e.RemoveClip(0, 0));
  SND_CHECK_EQ(e.ClipCount(0), 0u);
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(e.ClipCount(0), 1u);
  SND_REQUIRE(e.RemoveTrack(0));
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(e.TrackCount(), 1u);
}

SND_TEST(EventEditor, PatchAndSynthClipsBuild) {
  ProjectDocument doc;
  Load(doc);
  json::Value init;
  json::ParseError pe;
  SND_REQUIRE(json::Parse("{\"nodes\": [{\"id\": \"o\", \"kind\": \"oscillator\"}, {\"id\": \"out\", \"kind\": \"output\"}],"
                          " \"connections\": [{\"from\": \"o\", \"to\": \"out\"}]}",
                          &init, &pe));
  const std::string patch = doc.Create("patch", "Patch/Beep", &init);
  SND_REQUIRE(!patch.empty());
  EventEditor e(doc, G(8));
  SynthDesc s;
  s.kind = "patch";
  s.patch = patch;
  SND_REQUIRE(e.AddSynthClip(0, s, 0));
  SynthDesc o;
  o.kind = "oscillator";
  o.waveform = "pulse";
  o.duty = 0.25;
  o.frequencyHz = 220;
  SND_REQUIRE(e.AddSynthClip(0, o, 5));
  o.frequencyHz = 330;
  SND_REQUIRE(e.SetSynth(0, 2, o));
  SND_CHECK(!e.SetSynth(0, 0, o));  // an asset clip
  const DocBuild b = BuildDocument(doc);
  SND_CHECK_MSG(b.ok, "%s", Dump(b.issues).c_str());
}
