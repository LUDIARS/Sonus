// Independent tests of group M: broken input (properties H* of
// tool/test/indep_s7_model_properties.md). Nothing may crash (run under ASan / UBSan).
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "indep_s7_ops.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/project.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace indep_s7;
using namespace snd::tool;

namespace {

// Every reader of the document on every object (results are ignored: they must just return).
void TouchEverything(ProjectDocument& doc) {
  (void)CheckDocument(doc);
  const DocBuild b = BuildDocument(doc);
  (void)doc.BrokenReferences();
  (void)doc.PendingChanges();
  Project project;
  (void)project.Load(doc);
  MixerEditor mx(doc);
  (void)mx.GraphOrder();
  (void)mx.ApplySnapshots({{kSnap, 0.5}});
  std::vector<std::string> guids;
  for (const auto& o : doc.objects()) guids.push_back(o.guid);
  for (const auto& g : guids) {
    const DocObject* o = doc.Find(g);
    if (o == nullptr) continue;
    const std::string kind = o->kind();
    (void)doc.ReferencesOf(g);
    (void)doc.ReferencesTo(g);
    (void)LocateField(b, doc, g, "volumeDb");
    (void)LocateField(b, doc, g, "tracks[0].clips[0].startMs");
    if (kind == "event") {
      EventEditor e(doc, g);
      (void)e.ComputeTimeline();
      (void)e.UnusedMembers();
      (void)e.ClipNumberOf(0, 0, ClipNumber::StartMs);
      (void)e.Action(0, 0);
      (void)e.SetClipNumber(0, 0, ClipNumber::StartMs, 1);
      CurveEditor c(doc, g, "curves[0]");
      (void)c.Points();
      (void)c.Evaluate(1);
      (void)c.AddPoint(1, 1);
      CurveEditor dc(doc, g, "distanceCurves.volume");
      (void)dc.Points();
      (void)ComposeEvent(doc, g, {}, 3);
      SpatialPreview sp(doc, g);
      (void)sp.Compute();
      (void)sp.Audition();
      MusicEditor me(doc, g);
      (void)me.States();
      (void)me.Rules();
      (void)me.Unreachable();
      (void)me.Match("a", "b");
      (void)me.MinimumLookahead(MusicSettings{});
      (void)me.Preview("Explore", "Battle", 0.3, MusicSettings{});
      (void)me.AutoSort();
    } else if (kind == "bus") {
      (void)mx.SetVolume(g, -1);
      (void)mx.Children(g);
      (void)mx.EffectParam(g, 0, 0);
      (void)mx.SetEffectParam(g, 0, 0, 1);
    } else if (kind == "segment") {
      SegmentEditor s(doc, g);
      (void)s.Map();
      (void)s.ExitTick();
      (void)s.Markers();
      (void)s.ApplyGrid(120, 0, 4, 4);
    } else if (kind == "materialTable") {
      MaterialEditor m(doc, g);
      (void)m.Materials();
      (void)m.Fallbacks();
      (void)m.Resolve(0, 1, ContactKind::Impact);
      (void)m.Curves("metal");
      (void)m.AddMaterial("zz", 0);
    } else if (kind == "patch") {
      PatchEditor p(doc, g);
      (void)p.Nodes();
      (void)p.Connections();
      (void)p.Check();
      snd::buildtool::patch::Program prog;
      (void)p.Compiled(&prog);
      (void)p.AddNode("lfo", 0, 0);
    } else if (kind == "parameter") {
      ParameterEditor pe(doc, g);
      (void)pe.SetRange(0, 1);
      (void)pe.scope();
    } else if (kind == "playlist") {
      PlaylistEditor pl(doc, g);
      (void)pl.EntryCount();
      (void)pl.MoveEntry(0, 1);
    }
  }
  while (doc.Undo()) {
  }
  while (doc.Redo()) {
  }
}

}  // namespace

SND_TEST(IndepS7Robust, H1_EmptyProject) {
  ProjectDocument doc;
  SND_CHECK(doc.LoadFiles({}));
  SND_CHECK(doc.objects().empty());
  TouchEverything(doc);
  const DocBuild b = BuildDocument(doc);
  (void)b;
  SND_CHECK(doc.PendingChanges().empty());
  MixerEditor mx(doc);
  SND_CHECK_EQ(mx.MasterBus(), std::string());
  SND_CHECK(!mx.AddBus("Master", "").empty());  // the first bus is the master
  SND_CHECK_EQ(Image(doc).size(), 1u);
}

SND_TEST(IndepS7Robust, H1_WrongTypesEverywhere) {
  auto files = BaseFiles();
  files["events/W/1.json"] = R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000d1", "name": "W/1", "tracks": 5, "curves": "x", "cone": [1], "distanceCurves": {"volume": {"points": [[1]]}}, "music": {"states": 3, "transitions": [1, "x", {"from": 5}], "stingers": {}}})";
  files["events/W/2.json"] = R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000d2", "name": "W/2", "tracks": [{"clips": "x", "actions": 7}, 5, {"clips": [5, {"asset": 3, "startMs": "x", "random": 1, "layer": "y", "synth": []}]}], "curves": [{"points": 5}, {"points": [[1, 2, 3, 4]], "target": 9}, 7]})";
  files["buses/W.json"] = R"({"type": "bus", "guid": "00000000-0000-4000-8000-0000000000d3", "name": "W", "parent": 5, "sends": {"a": 1}, "effects": [{"params": "x"}, 3, {"type": 7, "params": [1, "b", null]}], "volumeDb": "loud"})";
  files["segments/W.json"] = R"({"type": "segment", "guid": "00000000-0000-4000-8000-0000000000d4", "name": "W", "asset": [], "bars": -3, "tempo": [{"tick": -5, "bpm": 0}, {"tick": "a"}, 4], "markers": [{"frame": -1}, 2], "stems": [{"channelFirst": 99999999999}]})";
  files["materials/W.json"] = R"({"type": "materialTable", "guid": "00000000-0000-4000-8000-0000000000d5", "name": "W", "materials": [1, "a", "a", null], "single": [1], "pairs": {"a": 1}, "footsteps": 5, "defaults": [], "curves": {"a": {"impulseToVelocity": [1]}}})";
  files["patches/W.json"] = R"({"type": "patch", "guid": "00000000-0000-4000-8000-0000000000d6", "name": "W", "nodes": [5, {"id": 3}, {"id": "a", "kind": "nope"}, {"id": "a", "kind": "output"}], "connections": [{"from": 1}, 7], "output": []})";
  files["parameters/W.json"] = R"({"type": "parameter", "guid": "00000000-0000-4000-8000-0000000000d7", "name": "W", "scope": "global", "min": "a", "max": [], "rampRate": {}})";
  files["playlists/W.json"] = R"({"type": "playlist", "guid": "00000000-0000-4000-8000-0000000000d8", "name": "W", "entries": [5, {"segment": 7, "weight": "x"}]})";
  files["snapshots/W.json"] = R"({"type": "snapshot", "guid": "00000000-0000-4000-8000-0000000000d9", "name": "W", "values": [{"kind": 5, "bus": []}, 1]})";
  files["types/1.json"] = R"({"type": 5, "guid": 7, "name": []})";
  files["types/2.json"] = R"({"type": "nonsense", "guid": "00000000-0000-4000-8000-0000000000da", "name": "X"})";
  files["types/3.json"] = R"({"type": "event"})";
  ProjectDocument doc;
  (void)LoadBase(doc, files);
  TouchEverything(doc);
  SND_CHECK(!CheckDocument(doc).empty());
}

SND_TEST(IndepS7Robust, H1_HugeNumbersAndDeepNesting) {
  auto files = BaseFiles();
  files["events/H/1.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000e1", "name": "H/1", "bank": ")") + kBank +
                             R"(", "priority": 1e400, "maxInstances": 18446744073709551616, "retriggerMs": -1e400, "maxDistance": 1e308,
 "tracks": [{"clips": [{"asset": ")" + kTone + R"(", "startMs": 1e308, "loopCount": 4294967296, "pitchCents": -1e308}]}]})";
  files["segments/H.json"] = std::string(R"({"type": "segment", "guid": "00000000-0000-4000-8000-0000000000e2", "name": "H", "asset": ")") + kTone +
                             R"(", "bars": 4294967295, "entryFrame": 18446744073709551615, "exitFrame": 9223372036854775807,
 "tempo": [{"tick": 18446744073709551615, "bpm": 1e300, "beatsPerBar": 4294967295, "beatUnit": 4294967295}, {"tick": 0, "bpm": 1e-300}]})";
  // Deep nesting in notes (the parser must not overflow the stack).
  std::string deep = "{\"type\": \"category\", \"guid\": \"00000000-0000-4000-8000-0000000000e3\", \"name\": \"Deep\", \"notes\": ";
  deep += std::string(100000, '[');
  deep += std::string(100000, ']');
  deep += "}";
  files["categories/Deep.json"] = deep;
  // A huge array.
  std::string big = "{\"type\": \"category\", \"guid\": \"00000000-0000-4000-8000-0000000000e4\", \"name\": \"Big\", \"notes\": [";
  for (int i = 0; i < 200000; ++i) big += i ? ",1" : "1";
  big += "]}";
  files["categories/Big.json"] = big;
  ProjectDocument doc;
  (void)LoadBase(doc, files);
  TouchEverything(doc);
  // The big and the deep category are kept and saved unchanged (TouchEverything edits others).
  SND_CHECK(doc.Find("00000000-0000-4000-8000-0000000000e4") != nullptr);
  for (const auto& c : doc.PendingChanges()) SND_CHECK(c.file != "categories/Big.json" && c.file != "categories/Deep.json");
  // Tempo map from huge points: finite answers.
  SegmentEditor s(doc, "00000000-0000-4000-8000-0000000000e2");
  const TempoMap m = s.Map();
  (void)m.SecondsAtTick(1e18);
  (void)m.Musical(1e18);
  (void)m.NextBar(1e18);
}

SND_TEST(IndepS7Robust, H1_BrokenJsonAndNonObjects) {
  auto files = BaseFiles();
  files["x/1.json"] = "";
  files["x/2.json"] = "{";
  files["x/3.json"] = "null";
  files["x/4.json"] = "\"str\"";
  files["x/5.json"] = std::string("{\"type\": \"bus\", \"name\": \"\xff\xfe\"}");
  files["x/6.json"] = "{\"a\": 1} trailing";
  files["x/7.json"] = std::string("\xEF\xBB\xBF\xEF\xBB\xBF{}");
  files["x/8.json"] = std::string("{\"type\": \"bus\", \"guid\": \"00000000-0000-4000-8000-0000000000f8\", \"name\": \"N\\u0000ul\"}");
  ProjectDocument doc;
  (void)LoadBase(doc, files);
  SND_CHECK(doc.unreadFiles().size() >= 6u);
  TouchEverything(doc);
  // Files that are not objects are never written (x/8.json is an object: TouchEverything edits it).
  for (const auto& c : doc.PendingChanges()) {
    if (c.file.rfind("x/", 0) == 0 && c.file != "x/8.json") {
      snd::test::Note("written: %s", c.file.c_str());
      SND_CHECK(false);
    }
  }
}

SND_TEST(IndepS7Robust, H2_MutatedFilesDoNotCrash) {
  const auto base = BaseFiles();
  std::vector<std::string> names;
  for (const auto& [k, v] : base) names.push_back(k);
  Rng r(31337);
  for (int iter = 0; iter < 120; ++iter) {
    auto files = base;
    std::string& t = files[names[r.Below(static_cast<std::uint32_t>(names.size()))]];
    const std::uint32_t edits = 1 + r.Below(4);
    for (std::uint32_t e = 0; e < edits && !t.empty(); ++e) {
      const std::uint32_t at = r.Below(static_cast<std::uint32_t>(t.size()));
      switch (r.Below(4)) {
        case 0:
          t.erase(at, 1 + r.Below(8));
          break;
        case 1:
          t[at] = "{}[]\",:0-e.9x\\"[r.Below(15)];
          break;
        case 2:
          t.insert(at, std::string(1 + r.Below(3), "{[\"1-"[r.Below(5)]));
          break;
        default:
          t[at] = static_cast<char>(r.Below(256));
          break;
      }
    }
    ProjectDocument doc;
    (void)LoadBase(doc, files);
    (void)CheckDocument(doc);
    if (iter % 4 == 0) TouchEverything(doc);
  }
}

SND_TEST(IndepS7Robust, H3_IndicesOutOfRange) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto image = Image(doc);
  EventEditor e(doc, kShot);
  const std::uint32_t big = 0xFFFFFFFFu;
  SND_CHECK(!e.MoveClip(big, big, 1));
  SND_CHECK(!e.MoveClipToTrack(0, 0, big, 1));
  SND_CHECK(!e.SetClipRandom(big, 0, RandomField::Pan, 0.1));
  SND_CHECK(!e.SetTrackBus(big, kSfx));
  SND_CHECK(!e.SetSynth(0, big, SynthDesc{}));
  (void)e.ClipNumberOf(big, big, ClipNumber::Weight);
  (void)e.ClipRandom(big, big, RandomField::Pan);
  (void)e.Action(big, big);
  SND_CHECK_EQ(e.ClipCount(big), 0u);
  SND_CHECK_EQ(e.ActionCount(big), 0u);
  MixerEditor m(doc);
  SND_CHECK(!m.RemoveSend(kSfx, big));
  SND_CHECK(!m.SetSendPreFader(kSfx, big, true));
  SND_CHECK(!m.RemoveEffect(kSfx, big));
  SND_CHECK(!m.SetEffectParam(kSfx, big, big, 1));
  SND_CHECK(!m.SetEffectFlags(kSfx, big, 1));
  SND_CHECK(!m.SetSnapshotValue(kSnap, big, SnapshotValue{}));
  (void)m.EffectParam(kSfx, big, big);
  CurveEditor c(doc, kShot, "curves[0]");
  SND_CHECK(!c.RemovePoint(big));
  SND_CHECK(!c.SetShape(big, 1));
  SegmentEditor s(doc, kSegA);
  SND_CHECK(!s.SetTempo(big, TempoPointDesc{}));
  SND_CHECK(!s.RemoveMarker(big));
  SND_CHECK(!s.SetStem(big, StemDesc{}));
  MusicEditor me(doc, kBgm);
  SND_CHECK(!me.SetRule(big, TransitionRule{}));
  SND_CHECK(!me.MoveRule(big, 0));
  SND_CHECK(!me.RemoveStinger(big));
  MaterialEditor mat(doc, kPhys);
  SND_CHECK(!mat.MoveMaterial(big, 0));
  PlaylistEditor pl(doc, kPlExplore);
  SND_CHECK(!pl.RemoveEntry(big));
  SND_CHECK(!pl.SetEntry(big, kSegA, 1, 1));
  SND_CHECK(!doc.MoveElement(kShot, "tracks", big, 0));
  SND_CHECK(!doc.Insert(kShot, "tracks", big, json::MakeObject()));
  SND_CHECK(!doc.Set(kShot, "tracks[4294967295].bus", String(kSfx)));
  SND_CHECK(!doc.Set(kShot, "tracks[99999999999999999999].bus", String(kSfx)));
  SND_CHECK(!doc.Set(kShot, "tracks[-1].bus", String(kSfx)));
  SND_CHECK(!doc.Set(kShot, "tracks[", String(kSfx)));
  SND_CHECK(!doc.Erase(kShot, "tracks[0].clips[9]"));
  SND_CHECK(!doc.Erase(kShot, ""));
  (void)FindPath(doc.Find(kShot)->doc, "tracks[0]..x");
  (void)FindPath(doc.Find(kShot)->doc, "[0]");
  (void)FindPath(doc.Find(kShot)->doc, "tracks[0][0][0]");
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  // Invalid editors (wrong kind or GUID) do nothing.
  EventEditor notEvent(doc, kSfx);
  SND_CHECK(!notEvent.valid());
  SND_CHECK(!notEvent.AddTrack(0));
  MaterialEditor notTable(doc, kShot);
  SND_CHECK(!notTable.valid());
  SND_CHECK(!notTable.AddMaterial("a", 0));
  PatchEditor notPatch(doc, "nope");
  SND_CHECK(!notPatch.valid());
  SND_CHECK(notPatch.AddNode("lfo", 0, 0).empty());
  SND_CHECK(Image(doc) == image);
}
