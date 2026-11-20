// The decisions of the review of group M (D-176 .. D-180): saving (negative zero, non-finite
// numbers, failures, "save as", reserved names), the edit rules (an unchanged value is success,
// GUID exhaustion, the last material, stem channels), the member -> bank table (covers the whole
// schema; LocateField and DiffBuilds agree; absent columns are not live), the read-only build
// from several threads, the music rules (time signature on bar lines, AutoSort with unreachable
// rows, approximate positions), compose the runtime ignores, the strength curves.
#include <cmath>
#include <filesystem>
#include <thread>

#include "doc_fixture.hpp"
#include "snd/buildtool/schema.hpp"
#include "snd/format/bank_check.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

std::map<std::string, std::string> RichFiles() {
  std::map<std::string, std::string> f = BaseFiles();
  f["assets/m.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(300) + "\", \"name\": \"m\", \"file\": \"audio/m.wav\"}";
  f["segments/S.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(301) + "\", \"name\": \"S\", \"asset\": \"m\", \"bars\": 2, "
                         "\"entryFrame\": 10, \"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}], "
                         "\"markers\": [{\"name\": \"K\", \"frame\": 100}], \"stems\": [{\"channelFirst\": 0, \"channelCount\": 2, "
                         "\"volumeCurve\": {\"parameter\": \"Speed\", \"target\": \"volume\", \"points\": [[0, -6], [10, 0]]}}]}";
  f["playlists/P.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(302) + "\", \"name\": \"P\", \"entries\": [{\"segment\": \"S\", \"weight\": 2}]}";
  f["events/Bgm.json"] = "{\"type\": \"event\", \"guid\": \"" + G(303) + "\", \"name\": \"Bgm\", \"bank\": \"Main\", \"music\": {"
                         "\"states\": [{\"name\": \"A\", \"playlist\": \"P\"}, {\"name\": \"B\", \"playlist\": \"P\"}],"
                         "\"transitions\": [{\"from\": \"A\", \"to\": \"B\", \"fadeOutBeats\": 1, \"timingCount\": 1}, {\"from\": \"*\", \"to\": \"*\", \"fadeInBeats\": 2}],"
                         "\"stingers\": [{\"name\": \"St\", \"event\": \"Shot\", \"timingCount\": 2}]}}";
  return f;
}

void LoadRich(ProjectDocument& doc) {
  LoadBase(doc, RichFiles());
  doc.SetBinaryFile("audio/m.wav", ToneWav(48000, 48000, 2));
}

}  // namespace

SND_TEST(Decisions, D176b_c_NegativeZeroAndNonFinite) {
  SND_CHECK_EQ(FormatNumber(-0.0), std::string("-0"));
  SND_CHECK(std::signbit(std::strtod(FormatNumber(-0.0).c_str(), nullptr)));
  SND_CHECK_EQ(FormatNumber(NAN), std::string("0"));  // the last guard
  ProjectDocument doc;
  LoadBase(doc);
  SND_REQUIRE(doc.SetNumber(G(3), "volumeDb", -0.0));
  json::Value v;
  json::ParseError pe;
  SND_REQUIRE(json::Parse(doc.TextOf(*doc.Find(G(3))), &v, &pe));
  SND_CHECK(std::signbit(NumberAt(v, "volumeDb", 1)));
  const std::size_t steps = doc.UndoCount();
  const std::string before = doc.TextOf(*doc.Find(G(3)));
  for (double bad : {NAN, INFINITY, -INFINITY}) {
    SND_CHECK(!doc.SetNumber(G(3), "volumeDb", bad));
    SND_CHECK(!doc.Set(G(3), "x-new", Number(bad)));
    json::Value arr = json::MakeArray();
    arr.items.push_back(Number(bad));
    SND_CHECK(!doc.Insert(G(3), "sends", 0, arr));
    SND_CHECK(!doc.Modify(G(3), [&](json::Value& o) {
      SetMember(o, "lodPriority", Number(bad), nullptr);
      return true;
    }));
    json::Value init = json::MakeObject();
    json::Add(init, "volumeDb", Number(bad));
    SND_CHECK(doc.Create("bus", "Bad", &init).empty());
  }
  SND_CHECK_EQ(doc.UndoCount(), steps);
  SND_CHECK(doc.TextOf(*doc.Find(G(3))) == before);
}

SND_TEST(Decisions, D177b_UnchangedValueIsSuccessWithoutAStep) {
  ProjectDocument doc;
  LoadBase(doc);
  SND_CHECK(doc.SetNumber(G(3), "volumeDb", -1.5));  // the loaded value
  SND_CHECK(doc.Set(G(8), "is3D", Bool(true)));
  SND_CHECK(doc.Rename(G(3), "Sfx"));
  EventEditor e(doc, G(8));
  SND_CHECK(e.SetLimitGroup(""));       // already none
  SND_CHECK(e.SetFlag(EventFlag::Stinger, false));
  SND_CHECK(e.SetSequence(SequenceKind::Polyphonic));
  MixerEditor m(doc);
  SND_CHECK(m.SetFlag(G(3), BusFlag::Meter, false));
  SND_CHECK(m.SetSendPreFader(G(3), 0, false));
  SND_CHECK(m.SetEffectBlob(G(3), 0, ""));
  SND_CHECK(m.SetCategoryParent(G(5), ""));
  SND_CHECK(ParameterEditor(doc, G(6)).SetSwitch(false));
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{0});
  SND_CHECK(doc.PendingChanges().empty());
  // A refused edit is still false.
  SND_CHECK(!doc.SetNumber(G(3), "tracks[0", 1));
}

SND_TEST(Decisions, D177c_GuidExhaustion) {
  ProjectDocument doc;
  LoadBase(doc);
  doc.SetRandom([] { return 0x1111111111111111ull; });
  const std::string first = doc.Create("event", "One");
  SND_REQUIRE(!first.empty());
  const std::size_t n = doc.objects().size(), steps = doc.UndoCount();
  SND_CHECK(doc.Create("event", "Two").empty());  // the generator only repeats a taken GUID
  SND_CHECK(doc.NewGuid().empty());
  SND_CHECK_EQ(doc.objects().size(), n);
  SND_CHECK_EQ(doc.UndoCount(), steps);
}

SND_TEST(Decisions, D176f_WindowsReservedNames) {
  ProjectDocument doc;
  LoadBase(doc);
  for (const char* name : {"CON", "nul", "Aux.x", "com1", "LPT9", "a/Prn"}) {
    const std::string g = doc.Create("event", name);
    SND_REQUIRE(!g.empty());
    const std::string file = doc.Find(g)->file;
    const std::string leaf = file.substr(file.rfind('/') + 1);
    SND_CHECK_MSG(leaf[0] == '_', "%s -> %s", name, file.c_str());
  }
  for (const char* name : {"CONSOLE", "COM10", "LPT0", "nulls"}) {
    const std::string g = doc.Create("event", name);
    SND_CHECK_MSG(doc.Find(g)->file == std::string("events/") + name + ".json", "%s", doc.Find(g)->file.c_str());
  }
}

SND_TEST(Decisions, D176d_SaveReportsFailuresAndKeepsThemPending) {
  TempDir dir("savefail");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  // A write that cannot happen: the target is a directory.
  SND_REQUIRE(doc.SetNumber(G(4), "volumeDb", -2));
  std::error_code ec;
  fs::remove(dir.path() / "buses/Reverb.json", ec);
  fs::create_directories(dir.path() / "buses/Reverb.json/x", ec);
  std::string err;
  SND_CHECK(!doc.Save(&err));
  SND_CHECK(err.find("buses/Reverb.json") != std::string::npos);
  SND_CHECK(doc.IsDirty());
  SND_CHECK(!doc.lastSave().ok && doc.lastSave().failures.size() == 1u);
  fs::remove_all(dir.path() / "buses/Reverb.json", ec);
  SND_CHECK(doc.Save());
  SND_CHECK(!doc.IsDirty());
  // A rename that moves the file: the new file is written, the old one cannot be deleted ->
  // false, the deletion stays pending, the next Save does it.
  SND_REQUIRE(doc.Rename(G(5), "UI"));
  fs::remove(dir.path() / "categories/SE.json", ec);
  fs::create_directories(dir.path() / "categories/SE.json/keep", ec);
  SND_CHECK(!doc.Save());
  SND_CHECK(dir.Exists("categories/UI.json"));
  SND_CHECK(doc.IsDirty());
  bool pendingDelete = false;
  for (const FileChange& c : doc.PendingChanges()) pendingDelete = pendingDelete || (c.op == FileChange::Op::Delete && c.file == "categories/SE.json");
  SND_CHECK(pendingDelete);
  fs::remove_all(dir.path() / "categories/SE.json", ec);
  SND_CHECK(doc.Save());
  SND_CHECK(!doc.IsDirty());
  // A failed write keeps the old file of a moved object.
  SND_REQUIRE(doc.Rename(G(5), "UI2"));
  fs::create_directories(dir.path() / "categories/UI2.json/x", ec);
  SND_CHECK(!doc.Save());
  SND_CHECK(dir.Exists("categories/UI.json"));
  fs::remove_all(dir.path() / "categories/UI2.json", ec);
  SND_CHECK(doc.Save());
  SND_CHECK(!dir.Exists("categories/UI.json") && dir.Exists("categories/UI2.json"));
}

SND_TEST(Decisions, D176e_SaveAsCopiesTheMaterial) {
  TempDir a("saveas-a"), b("saveas-b");
  std::map<std::string, std::string> f = BaseFiles();
  f["assets/out.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(310) + "\", \"name\": \"out\", \"file\": \"../shared/x.wav\"}";
  WriteAll(a, f);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(a.path().string()));
  SND_REQUIRE(doc.Remove(G(310)));  // not used by the build; only to list the outside reference below
  SND_REQUIRE(doc.Undo());
  SND_REQUIRE(doc.SaveTo(b.path().string()));
  SND_CHECK(b.Exists("audio/tone.wav"));
  SND_CHECK(doc.lastSave().outside.size() == 1u && doc.lastSave().outside[0] == "../shared/x.wav");
  SND_CHECK_EQ(doc.directory(), b.path().string());
  SND_CHECK(!doc.IsDirty());
  doc.Remove(G(310), true);
  const DocBuild built = BuildDocument(doc);
  SND_CHECK_MSG(built.ok, "%s", Dump(built.issues).c_str());
}

SND_TEST(Decisions, D178b_EverySchemaMemberHasAnEffect) {
  std::uint32_t n = 0;
  for (const std::string& kind : snd::buildtool::SchemaKinds()) {
    for (const std::string& path : snd::buildtool::SchemaMembers(kind)) {
      const FieldEffect e = ClassifyMember(kind, path);
      SND_CHECK_MSG(e != FieldEffect::Unknown, "%s %s", kind.c_str(), path.c_str());
      ++n;
    }
  }
  SND_CHECK(n > 300u);
  SND_CHECK(ClassifyMember("event", "tracks[0].clips[1].notes.x") == FieldEffect::NotInBank);
  SND_CHECK(ClassifyMember("patch", "nodes[3].position") == FieldEffect::NotInBank);
  SND_CHECK(ClassifyMember("event", "music.transitions[2].fadeInBeats") == FieldEffect::Column);
  SND_CHECK(ClassifyMember("event", "nosuch") == FieldEffect::Unknown);
}

SND_TEST(Decisions, D178b_c_LocateFieldAgreesWithDiffBuilds) {
  ProjectDocument doc;
  LoadRich(doc);
  SND_REQUIRE(SetDistanceCurve(doc, G(8), CurveTarget::Lowpass, {{0, 20000}, {50, 500}}));
  // Members of the music event, the segment, the distance curves, stems, the clip, the bus...
  const struct {
    std::string guid;
    const char* path;
    double value;
  } cases[] = {
      {G(303), "music.transitions[0].fadeOutBeats", 3},  {G(303), "music.transitions[1].fadeInBeats", 4},
      {G(303), "music.transitions[0].timingCount", 5},   {G(303), "music.stingers[0].timingCount", 3},
      {G(301), "tempo[0].bpm", 90},                      {G(301), "markers[0].frame", 200},
      {G(301), "bars", 3},                               {G(301), "entryFrame", 20},
      {G(301), "stems[0].channelCount", 1},              {G(301), "stems[0].volumeCurve.points[1][1]", -3},
      {G(302), "entries[0].weight", 5},                  {G(8), "distanceCurves.volume.points[1][1]", -20},
      {G(8), "distanceCurves.lowpass.points[0][1]", 15000}, {G(8), "tracks[0].clips[0].volumeDb", -9},
      {G(8), "tracks[0].clips[0].pitchCents", 50},       {G(8), "comboWindowMs", 30},
      {G(3), "volumeDb", -4},                            {G(3), "sends[0].levelDb", -20},
  };
  for (const auto& c : cases) {
    ProjectDocument before = doc;
    const DocBuild a = BuildDocument(before);
    SND_REQUIRE_MSG(a.ok, "%s", Dump(a.issues).c_str());
    const std::vector<CellRef> loc = LocateField(a, before, c.guid, c.path);
    SND_REQUIRE_MSG(!loc.empty(), "%s: no cell", c.path);
    for (const CellRef& r : loc) {
      if (!a.FindBank(r.bank, r.language)->view()->HasColumn(r.chunk, r.column)) {
        SND_CHECK_MSG(!r.liveWritable, "%s: absent column reported live", c.path);
      }
    }
    SND_REQUIRE(doc.SetNumber(c.guid, c.path, c.value));
    const DocBuild b = BuildDocument(doc);
    SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
    const LiveDelta d = DiffBuilds(a, b);
    // The member's cell changed: either a live value at the located address, or a chunk.
    bool live = true;
    for (const CellRef& r : loc) live = live && r.liveWritable;
    if (live) {
      SND_CHECK_MSG(d.kind == EditClass::Value, "%s: located live, DiffBuilds says %d", c.path, static_cast<int>(d.kind));
      for (const ValueEdit& v : d.values) {
        bool same = false;
        for (const CellRef& r : loc) same = same || (r.chunk == v.cell.chunk && r.column == v.cell.column && r.row == v.cell.row);
        SND_CHECK_MSG(same, "%s: DiffBuilds edits another cell", c.path);
      }
    } else {
      bool chunk = false;
      for (const ChunkEdit& e : d.chunks) {
        for (const CellRef& r : loc) chunk = chunk || e.fourcc == r.chunk;
      }
      SND_CHECK_MSG(d.kind == EditClass::Structure && chunk, "%s: not live, but DiffBuilds says %d", c.path,
                    static_cast<int>(d.kind));
    }
    // After the edit the located cell holds the value.
    const std::vector<CellRef> after = LocateField(b, doc, c.guid, c.path);
    SND_REQUIRE(!after.empty());
    const double got = bf::check::FindColumnInfo(after[0].chunk, after[0].column)->type == bf::ColumnType::F32
                           ? BitsToFloat(after[0].bits)
                           : static_cast<double>(after[0].bits);
    SND_CHECK_MSG(std::fabs(got - c.value) < 1e-3 || std::string(c.path) == "markers[0].frame" || std::string(c.path) == "entryFrame",
                  "%s: %g", c.path, got);
  }
}

SND_TEST(Decisions, D178d_ReadingABuildFromThreads) {
  ProjectDocument doc;
  LoadRich(doc);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE(b.ok);
  const DocBuild copy = b;
  std::uint32_t rows[4] = {};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&, t] {
      const DocBuild& use = (t % 2) != 0 ? copy : b;
      for (int i = 0; i < 200; ++i) rows[t] += use.banks[0].view()->Rows(bf::chunk::EVNT);
    });
  }
  for (std::thread& t : ts) t.join();
  SND_CHECK(rows[0] == rows[1] && rows[1] == rows[2] && rows[0] > 0u);
}

SND_TEST(Decisions, D177d_LastMaterialAndStemChannels) {
  ProjectDocument doc;
  LoadRich(doc);
  json::Value init;
  json::ParseError pe;
  SND_REQUIRE(json::Parse("{\"bank\": \"Main\", \"materials\": [\"a\", \"b\"]}", &init, &pe));
  MaterialEditor m(doc, doc.Create("materialTable", "Phys", &init));
  SND_CHECK(m.RemoveMaterial("a"));
  SND_CHECK(!m.RemoveMaterial("b"));  // the last one stays
  SND_CHECK_EQ(m.Materials().size(), std::size_t{1});
  SegmentEditor s(doc, G(301));  // m.wav: stereo
  SND_CHECK(!s.SetStem(0, {1, 2, ""}));
  SND_CHECK(!s.AddStem({0, 2, G(7)}));  // tone.wav: mono
  SND_CHECK(s.AddStem({0, 1, G(7)}));
  SND_CHECK(!s.SetStem(1, {0, 2, G(7)}));
  // Unreadable material: accepted, reported.
  doc.SetBinaryFile("audio/m.wav", {1, 2, 3});
  SND_CHECK(s.SetStem(0, {4, 4, ""}));
  bool reported = false;
  for (const std::string& p : s.Problems()) reported = reported || p.rfind("stems[0]", 0) == 0;
  SND_CHECK(reported);
}

SND_TEST(Decisions, D179_MusicRules) {
  ProjectDocument doc;
  LoadRich(doc);
  SegmentEditor s(doc, G(301));
  SND_CHECK(!s.AddTempo({1000, 120, 3, 4}));  // a signature change inside bar 0
  SND_CHECK(s.AddTempo({1000, 90, 4, 4}));    // a tempo change anywhere
  SND_CHECK(s.AddTempo({3840, 90, 3, 4}));    // on a bar line
  SND_CHECK(!s.SetTempo(2, {4000, 90, 3, 4}));
  SND_CHECK(s.Problems().empty());
  // A mid-bar change written by hand is a problem; the map follows the runtime anyway.
  json::Value t;
  json::ParseError pe;
  SND_REQUIRE(json::Parse("[{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}, {\"tick\": 1000, \"bpm\": 120, \"beatsPerBar\": 3, \"beatUnit\": 4}]", &t, &pe));
  SND_REQUIRE(doc.Set(G(301), "tempo", t));
  SND_CHECK_EQ(s.Problems().size(), std::size_t{1});
  // 3/2: beats of 1920 ticks; positions are approximate.
  const TempoMap half({{0, 120, 3, 2}});
  SND_CHECK(half.MusicalIsApproximate(1500));
  SND_CHECK(!TempoMap({{0, 120, 4, 4}}).MusicalIsApproximate(1500));
  // AutoSort refuses a table with unreachable rows.
  MusicEditor me(doc, G(303));
  TransitionRule r;
  r.from = "B";
  r.to = "A";
  SND_REQUIRE(me.AddRule(r, 2));  // after "* -> *": unreachable
  std::string why;
  SND_CHECK(!me.AutoSort(&why));
  SND_CHECK(why.find("transitions[2]") != std::string::npos);
  SND_REQUIRE(me.RemoveRule(2));
  SND_CHECK(me.AutoSort(&why));
}

SND_TEST(Decisions, D179b_ComposeTheRuntimeIgnores) {
  ProjectDocument doc;
  LoadBase(doc);
  CurveEditor c(doc, G(8), "curves[0]");  // pitch
  SND_CHECK(!c.ComposeIgnored());
  SND_CHECK(c.SetCompose(CurveCompose::Multiply));  // accepted
  SND_CHECK(c.ComposeIgnored());
  const ComposeResult r = ComposeEvent(doc, G(8), {}, 0);
  SND_CHECK(r.composeIgnored.size() == 1u && r.composeIgnored[0] == 0u);
  SND_CHECK_NEAR(r.pitchCents, 120.0, 1e-3);  // still added, as the runtime does
  SND_CHECK(c.SetTarget(CurveTarget::Spread));
  SND_CHECK(!c.ComposeIgnored());
  SND_CHECK(ComposeIgnored(CurveTarget::Pan) && !ComposeIgnored(CurveTarget::Send));
}

SND_TEST(Decisions, D180b_StrengthIsFiniteForAnyInput) {
  const StrengthCurves c;
  for (float x : {NAN, INFINITY, -INFINITY}) {
    const StrengthResult r = EvaluateStrength(c, x, x, x);
    SND_CHECK(std::isfinite(r.velocity) && std::isfinite(r.pitchCents) && std::isfinite(r.volumeDb));
    SND_CHECK_EQ(r.layer, -1);
  }
}
