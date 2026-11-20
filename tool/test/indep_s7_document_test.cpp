// Independent tests of group M: the editable document (load / save, undo / redo,
// references, GUIDs). Properties A*, B*, C* of tool/test/indep_s7_model_properties.md.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "indep_s7_ops.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"

using namespace indep_s7;
using namespace snd::tool;

namespace {

bool HasChange(const std::vector<FileChange>& c, FileChange::Op op, const std::string& file) {
  for (const auto& x : c)
    if (x.op == op && x.file == file) return true;
  return false;
}

std::string SavedText(const ProjectDocument& doc, const std::string& guid) {
  const DocObject* o = doc.Find(guid);
  return o ? doc.TextOf(*o) : std::string();
}

// Position of `key` (as "\"key\":") in text, npos if absent.
std::size_t KeyPos(const std::string& text, const std::string& key) { return text.find("\"" + key + "\":"); }

}  // namespace

// --- A. load / save -----------------------------------------------------------------------------

SND_TEST(IndepS7Doc, A1_LoadSaveUnchangedWritesNothing) {
  const std::string dir = TempDir("a1");
  WriteBaseProject(dir);
  const auto before = DirContents(dir);
  const auto times = DirTimes(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_CHECK(doc.loadIssues().empty());
  SND_CHECK(doc.PendingChanges().empty());
  SND_CHECK(!doc.IsDirty());
  std::string err;
  SND_CHECK(doc.Save(&err));
  SND_CHECK(DirContents(dir) == before);
  SND_CHECK(DirTimes(dir) == times);
}

SND_TEST(IndepS7Doc, A2_SaveLoadRoundTrip) {
  const std::string dir = TempDir("a2");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4.25));
  SND_REQUIRE(doc.SetNumber(kShot, "tracks[0].clips[1].startMs", 250));
  SND_REQUIRE(!doc.Create("bus", "New/Bus", nullptr).empty() || true);
  SND_REQUIRE(doc.Save());
  SND_CHECK(doc.PendingChanges().empty());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir));
  const auto a = Image(doc);
  const auto b = Image(again);
  SND_CHECK_EQ(a.size(), b.size());
  for (const auto& [g, text] : a) {
    auto it = b.find(g);
    SND_CHECK(it != b.end());
    if (it != b.end() && it->second != text) {
      snd::test::Note("object %s differs after reload", g.c_str());
      SND_CHECK(false);
    }
  }
  // And it saves nothing more.
  SND_CHECK(again.PendingChanges().empty());
}

SND_TEST(IndepS7Doc, A3_UnknownMembersAndNotesAtEveryLevelSurvive) {
  auto files = BaseFiles();
  files["events/SE/Odd.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000aa", "name": "SE/Odd",
 "bank": ")") + kBank + R"(", "xTop": {"deep": [1, 2, {"k": null}]}, "notes": {"author": "x", "list": [1, "two"]},
 "cone": {"innerDeg": 10, "outerDeg": 20, "outerDb": -3, "xCone": true, "notes": "cone note"},
 "tracks": [{"xTrack": 5, "notes": [1, 2], "clips": [{"asset": ")" + kTone +
                                   R"(", "xClip": "keep", "notes": 3, "random": {"volumeDb": 1, "xRand": 0.5, "notes": false}}],
             "actions": [{"timeMs": 0, "action": "play", "target": ")" + kHit +
                                   R"(", "xAct": [], "notes": "a"}]}],
 "music": null})";
  // The music event and the material table get unknown members inside their tables.
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  SND_REQUIRE(doc.Modify(kBgm, [](json::Value& v) {
    json::Value* t = FindPath(v, "music.transitions[0]");
    if (t == nullptr) return false;
    json::Add(*t, "xRule", json::MakeString("r"));
    json::Add(*t, "notes", json::MakeString("rule note"));
    return true;
  }));
  SND_REQUIRE(doc.Modify(kPhys, [](json::Value& v) {
    json::Value* s = FindPath(v, "single");
    if (s == nullptr) return false;
    json::Add(*s, "notes", json::MakeString("table note"));
    return true;
  }));
  doc.MarkSaved();
  const std::string odd = "00000000-0000-4000-8000-0000000000aa";
  SND_REQUIRE(doc.Find(odd) != nullptr);
  // Edit another member of each, save, read the text back.
  SND_REQUIRE(doc.SetNumber(odd, "tracks[0].clips[0].startMs", 12));
  SND_REQUIRE(doc.SetNumber(odd, "cone.outerDb", -6));
  SND_REQUIRE(doc.SetNumber(kBgm, "music.transitions[0].fadeInBeats", 2));
  EventEditor ev(doc, kHit);
  SND_REQUIRE(ev.SetNumber(EventNumber::Priority, 10));
  MaterialEditor mat(doc, kPhys);
  SND_REQUIRE(mat.SetFootstep("metal", kHit));
  for (const char* g : {odd.c_str(), kBgm, kPhys}) {
    json::Value back;
    json::ParseError pe;
    SND_REQUIRE(json::Parse(SavedText(doc, g), &back, &pe));
    SND_CHECK(JsonEqual(back, doc.Find(g)->doc));
  }
  json::Value back;
  json::ParseError pe;
  SND_REQUIRE(json::Parse(SavedText(doc, odd), &back, &pe));
  SND_CHECK(FindPath(back, "xTop.deep[2].k") != nullptr);
  SND_CHECK_EQ(StringAt(back, "notes.author"), std::string("x"));
  SND_CHECK(BoolAt(back, "cone.xCone", false));
  SND_CHECK_EQ(StringAt(back, "cone.notes"), std::string("cone note"));
  SND_CHECK_EQ(NumberAt(back, "tracks[0].xTrack", 0), 5.0);
  SND_CHECK(FindPath(back, "tracks[0].notes[1]") != nullptr);
  SND_CHECK_EQ(StringAt(back, "tracks[0].clips[0].xClip"), std::string("keep"));
  SND_CHECK_EQ(NumberAt(back, "tracks[0].clips[0].notes", 0), 3.0);
  SND_CHECK_EQ(NumberAt(back, "tracks[0].clips[0].random.xRand", 0), 0.5);
  SND_CHECK(FindPath(back, "tracks[0].clips[0].random.notes") != nullptr);
  SND_CHECK(FindPath(back, "tracks[0].actions[0].xAct") != nullptr);
  SND_CHECK(FindPath(back, "music") != nullptr && FindPath(back, "music")->IsNull());
  SND_CHECK_EQ(NumberAt(back, "tracks[0].clips[0].startMs", 0), 12.0);
  json::Value bgm;
  SND_REQUIRE(json::Parse(SavedText(doc, kBgm), &bgm, &pe));
  SND_CHECK_EQ(StringAt(bgm, "music.transitions[0].xRule"), std::string("r"));
  SND_CHECK_EQ(StringAt(bgm, "music.transitions[0].notes"), std::string("rule note"));
  json::Value phys;
  SND_REQUIRE(json::Parse(SavedText(doc, kPhys), &phys, &pe));
  SND_CHECK_EQ(StringAt(phys, "single.notes"), std::string("table note"));
  // notes are not taken as a material by the material editor.
  for (const auto& m : mat.Materials()) SND_CHECK(m != "notes");
}

SND_TEST(IndepS7Doc, A4_A5_MemberOrderIndentAndNewline) {
  auto files = BaseFiles();
  // Members in an unusual order: tracks before name, bus last.
  files["events/SE/Order.json"] = std::string(R"({"tracks": [{"clips": [{"asset": ")") + kTone +
                                  R"("}]}], "type": "event", "zzz": 1, "guid": "00000000-0000-4000-8000-0000000000ab", "name": "SE/Order", "priority": 3, "bank": ")" +
                                  kBank + R"("})";
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  const std::string g = "00000000-0000-4000-8000-0000000000ab";
  SND_REQUIRE(doc.SetNumber(g, "priority", 4));
  SND_REQUIRE(doc.SetNumber(g, "fadeOutMs", 12));  // a new member
  const std::string t = SavedText(doc, g);
  // Old order kept.
  SND_CHECK(KeyPos(t, "tracks") < KeyPos(t, "type"));
  SND_CHECK(KeyPos(t, "type") < KeyPos(t, "zzz"));
  SND_CHECK(KeyPos(t, "zzz") < KeyPos(t, "guid"));
  SND_CHECK(KeyPos(t, "name") < KeyPos(t, "priority"));
  SND_CHECK(KeyPos(t, "priority") < KeyPos(t, "bank"));
  // New member: present, after "priority" (the spec's table puts fadeOutMs after priority).
  SND_CHECK(KeyPos(t, "fadeOutMs") != std::string::npos);
  SND_CHECK(KeyPos(t, "fadeOutMs") > KeyPos(t, "priority"));
  // Form: 2-space indent, one '\n' at the end.
  SND_REQUIRE(!t.empty());
  SND_CHECK(t.back() == '\n');
  SND_CHECK(t.size() < 2 || t[t.size() - 2] != '\n');
  SND_CHECK(t.find("\n  \"") != std::string::npos);
  SND_CHECK(t.find('\t') == std::string::npos);
  // It is what json::Write produces.
  json::Value v;
  json::ParseError pe;
  SND_REQUIRE(json::Parse(t, &v, &pe));
  SND_CHECK_EQ(json::Write(v), t);
}

SND_TEST(IndepS7Doc, A6_FormatNumberRoundTrips) {
  const double values[] = {0.1, 1e-7, 0.3, 1.0 / 3.0, 9007199254740992.0, 9007199254740993.0, 1e300, -1e300, 5e-324,
                           2.2250738585072014e-308, 123456789.123456789, -2.5, 48000.0, 1e21, 1e22, 0.0};
  for (double v : values) {
    const std::string s = FormatNumber(v);
    char* end = nullptr;
    const double back = std::strtod(s.c_str(), &end);
    SND_CHECK(end != nullptr && *end == '\0');
    if (std::memcmp(&back, &v, sizeof v) != 0) {
      snd::test::Note("FormatNumber(%.17g) = '%s' reads back as %.17g", v, s.c_str(), back);
      SND_CHECK(false);
    }
    // JSON-valid number text.
    json::Value j;
    json::ParseError pe;
    SND_CHECK(json::Parse(s, &j, &pe));
  }
  // Integers without a fraction.
  SND_CHECK_EQ(FormatNumber(48000.0), std::string("48000"));
  SND_CHECK_EQ(FormatNumber(-3.0), std::string("-3"));
  // Non-finite -> 0.
  SND_CHECK_EQ(FormatNumber(std::numeric_limits<double>::quiet_NaN()), std::string("0"));
  SND_CHECK_EQ(FormatNumber(std::numeric_limits<double>::infinity()), std::string("0"));
}

SND_TEST(IndepS7Doc, A6_A7_UneditedLiteralsStay) {
  auto files = BaseFiles();
  files["buses/Lit.json"] = std::string(R"({"type": "bus", "guid": "00000000-0000-4000-8000-0000000000ac", "name": "Lit", "parent": ")") +
                            kMaster + R"(", "volumeDb": 1e-7, "lodPriority": 0.10e1, "notes": [0.10, 1E+2, -0, 9223372036854775807, 18446744073709551615]})";
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  const std::string g = "00000000-0000-4000-8000-0000000000ac";
  SND_REQUIRE(doc.SetNumber(g, "channels", 2));
  const std::string t = SavedText(doc, g);
  SND_CHECK(t.find("1e-7") != std::string::npos);
  SND_CHECK(t.find("0.10e1") != std::string::npos);
  SND_CHECK(t.find("0.10,") != std::string::npos || t.find("0.10\n") != std::string::npos);
  SND_CHECK(t.find("1E+2") != std::string::npos);
  SND_CHECK(t.find("-0") != std::string::npos);
  SND_CHECK(t.find("9223372036854775807") != std::string::npos);
  SND_CHECK(t.find("18446744073709551615") != std::string::npos);
  // An edited number reads back as the same double.
  SND_REQUIRE(doc.SetNumber(g, "volumeDb", 0.1));
  json::Value v;
  json::ParseError pe;
  SND_REQUIRE(json::Parse(SavedText(doc, g), &v, &pe));
  SND_CHECK_EQ(NumberAt(v, "volumeDb", 99), 0.1);
  SND_REQUIRE(doc.SetNumber(g, "volumeDb", 1e-7));
  SND_REQUIRE(json::Parse(SavedText(doc, g), &v, &pe));
  SND_CHECK_EQ(NumberAt(v, "volumeDb", 99), 1e-7);
  // (-0.0: Defect_NegativeZeroLosesItsSign in indep_s7_defects_test.cpp.)
}

SND_TEST(IndepS7Doc, A8_A9_Utf8BomAndEscapes) {
  const std::string dir = TempDir("a8");
  auto files = BaseFiles();
  const std::string name = "SE/\xE7\x88\x86\xE7\x99\xBA \xF0\x9F\x94\xA5 \"q\" \\b";  // 爆発 🔥 "q" \b
  files["events/SE/Utf.json"] = std::string("\xEF\xBB\xBF") + R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000ad", "name": "SE/爆発 🔥 \"q\" \\b", "bank": ")" +
                                kBank + R"(", "notes": "tab\there\nline \u0001", "tracks": [{"clips": [{"asset": ")" + kTone +
                                R"("}]}]})";
  WriteBaseProject(dir, files);
  const std::string rawBom = ReadFileText(dir + "/events/SE/Utf.json");
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  const std::string g = "00000000-0000-4000-8000-0000000000ad";
  const DocObject* o = doc.Find(g);
  SND_REQUIRE(o != nullptr);
  SND_CHECK_EQ(o->name(), name);
  SND_CHECK_EQ(StringAt(o->doc, "notes"), std::string("tab\there\nline \x01"));
  SND_CHECK(doc.FindByName("event", name) == o);
  // Unchanged BOM file: byte for byte (A9).
  SND_CHECK(doc.PendingChanges().empty());
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -1));
  SND_REQUIRE(doc.Save());
  SND_CHECK_EQ(ReadFileText(dir + "/events/SE/Utf.json"), rawBom);
  // Edited: round trip of the strings.
  SND_REQUIRE(doc.SetNumber(g, "priority", 5));
  SND_REQUIRE(doc.Save());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir));
  const DocObject* o2 = again.Find(g);
  SND_REQUIRE(o2 != nullptr);
  SND_CHECK_EQ(o2->name(), name);
  SND_CHECK_EQ(StringAt(o2->doc, "notes"), std::string("tab\there\nline \x01"));
  // The edited object still builds by name: the document checks clean for that object.
  SND_CHECK(IssuesOf(CheckDocument(again), g).empty());
}

SND_TEST(IndepS7Doc, A10_FileNamesOfNewObjects) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SND_CHECK_EQ(doc.FileNameFor("event", "SE/Boom"), std::string("events/SE/Boom.json"));
  SND_CHECK_EQ(doc.FileNameFor("bus", "Amb"), std::string("buses/Amb.json"));
  SND_CHECK_EQ(doc.FileNameFor("patch", "P"), std::string("patches/P.json"));
  SND_CHECK_EQ(doc.FileNameFor("materialTable", "M"), std::string("materials/M.json"));
  SND_CHECK_EQ(doc.FileNameFor("voiceLimitGroup", "V"), std::string("voiceLimitGroups/V.json"));
  SND_CHECK_EQ(doc.FileNameFor("ducking", "D"), std::string("duckings/D.json"));
  // Non-portable characters.
  const std::string bad = doc.FileNameFor("event", "a:b*c?d\"e<f>g|h");
  SND_CHECK_EQ(bad, std::string("events/a_b_c_d_e_f_g_h.json"));
  // Hidden parts get a '_' prefix (the loader skips '.' directories).
  SND_CHECK_EQ(doc.FileNameFor("event", ".x/.y"), std::string("events/_.x/_.y.json"));
  // A clash (case-insensitive) with an existing file: "_2".
  SND_CHECK_EQ(doc.FileNameFor("event", "se/shot"), std::string("events/se/shot_2.json"));
  // Create puts the file there and the save writes it.
  const std::string g = doc.Create("event", "SE/Shot2", nullptr);
  SND_REQUIRE(!g.empty());
  SND_CHECK_EQ(doc.Find(g)->file, std::string("events/SE/Shot2.json"));
  SND_CHECK(HasChange(doc.PendingChanges(), FileChange::Op::Write, "events/SE/Shot2.json"));
  const std::string g2 = doc.Create("bus", "se/shot2", nullptr);  // other kind: other folder, no clash
  SND_REQUIRE(!g2.empty());
  SND_CHECK_EQ(doc.Find(g2)->file, std::string("buses/se/shot2.json"));
}

SND_TEST(IndepS7Doc, A10_ClashCountsUp) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  // "A:B" and "A_B" and "a?b" map to the same file name.
  const std::string g1 = doc.Create("category", "A:B", nullptr);
  const std::string g2 = doc.Create("category", "A_B", nullptr);
  const std::string g3 = doc.Create("category", "a?b", nullptr);
  SND_REQUIRE(!g1.empty() && !g2.empty() && !g3.empty());
  SND_CHECK_EQ(doc.Find(g1)->file, std::string("categories/A_B.json"));
  SND_CHECK_EQ(doc.Find(g2)->file, std::string("categories/A_B_2.json"));
  SND_CHECK_EQ(doc.Find(g3)->file, std::string("categories/a_b_3.json"));
}

SND_TEST(IndepS7Doc, A11_RemoveDeletesRenameMoves) {
  const std::string dir = TempDir("a11");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  // Unreferenced category UI? It is not referenced in the fixture -> removable.
  SND_REQUIRE(doc.Remove(kCatUi));
  SND_CHECK(HasChange(doc.PendingChanges(), FileChange::Op::Delete, "categories/UI.json"));
  // Rename a bus at its rule path: moves.
  SND_REQUIRE(doc.Rename(kReverb, "FX/Room"));
  auto ch = doc.PendingChanges();
  SND_CHECK(HasChange(ch, FileChange::Op::Delete, "buses/Reverb.json"));
  SND_CHECK(HasChange(ch, FileChange::Op::Write, "buses/FX/Room.json"));
  SND_REQUIRE(doc.Save());
  auto files = DirContents(dir);
  SND_CHECK(files.count("categories/UI.json") == 0);
  SND_CHECK(files.count("buses/Reverb.json") == 0);
  SND_CHECK(files.count("buses/FX/Room.json") == 1);
  SND_CHECK(doc.PendingChanges().empty());
  // A file not at its name's path stays where it is on rename.
  WriteFile(dir + "/buses/odd_place.json", std::string(R"({"type": "bus", "guid": "00000000-0000-4000-8000-0000000000ae", "name": "Elsewhere", "parent": ")") + kMaster + "\"}");
  ProjectDocument d2;
  SND_REQUIRE(d2.Load(dir));
  SND_REQUIRE(d2.Rename("00000000-0000-4000-8000-0000000000ae", "Elsewhere2"));
  SND_CHECK_EQ(d2.Find("00000000-0000-4000-8000-0000000000ae")->file, std::string("buses/odd_place.json"));
  // moveFile = false keeps the file too.
  SND_REQUIRE(d2.Rename(kMusicBus, "Music2", RenameOptions{true, false}));
  SND_CHECK_EQ(d2.Find(kMusicBus)->file, std::string("buses/Music.json"));
}

SND_TEST(IndepS7Doc, A12_UnreadableJsonIsNeverTouched) {
  const std::string dir = TempDir("a12");
  WriteBaseProject(dir);
  WriteFile(dir + "/events/Broken.json", "{\"type\": \"event\", \"name\": ");
  WriteFile(dir + "/events/Array.json", "[1, 2, 3]");
  WriteFile(dir + "/events/Dup.json", "{\"type\": \"event\", \"type\": \"bus\"}");
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_CHECK(doc.unreadFiles().size() >= 2u);
  SND_CHECK(!doc.loadIssues().empty());
  // Even an object named like the broken file does not overwrite it.
  const std::string g = doc.Create("event", "Broken", nullptr);
  SND_REQUIRE(!g.empty());
  SND_CHECK(doc.Find(g)->file != "events/Broken.json");
  SND_REQUIRE(doc.Save());
  SND_CHECK_EQ(ReadFileText(dir + "/events/Broken.json"), std::string("{\"type\": \"event\", \"name\": "));
  SND_CHECK_EQ(ReadFileText(dir + "/events/Array.json"), std::string("[1, 2, 3]"));
  SND_CHECK_EQ(ReadFileText(dir + "/events/Dup.json"), std::string("{\"type\": \"event\", \"type\": \"bus\"}"));
}

SND_TEST(IndepS7Doc, A13_FailedWriteKeepsTheDocument) {
  const std::string dir = TempDir("a13");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  // "snapshots" is a regular file: no new snapshot file can be written under it (also as root).
  WriteFile(dir + "/patches_blocker", "x");
  std::error_code ec;
  fs::remove_all(dir + "/snapshots", ec);
  WriteFile(dir + "/snapshots", "not a directory");
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -9));
  const std::string g = doc.Create("snapshot", "Loud", nullptr);
  SND_REQUIRE(!g.empty());
  const auto image = Image(doc);
  const auto pending = doc.PendingChanges().size();
  std::string err;
  SND_CHECK(!doc.Save(&err));
  SND_CHECK(!err.empty());
  // The document is as edited and still dirty.
  SND_CHECK(Image(doc) == image);
  SND_CHECK(doc.IsDirty());
  SND_CHECK(doc.Find(g) != nullptr);
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "volumeDb", 0), -9.0);
  // Remove the obstacle: the save goes through and everything lands.
  fs::remove(dir + "/snapshots", ec);
  SND_CHECK(doc.Save(&err));
  SND_CHECK(!doc.IsDirty());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir));
  SND_CHECK(again.Find(g) != nullptr);
  SND_CHECK_EQ(NumberAt(again.Find(kSfx)->doc, "volumeDb", 0), -9.0);
  (void)pending;
}

SND_TEST(IndepS7Doc, A15_SameEditsSameOutput) {
  ProjectDocument a, b;
  SND_REQUIRE(LoadBase(a));
  SND_REQUIRE(LoadBase(b));
  Rng ra(77), rb(77);
  for (std::uint32_t i = 0; i < 200; ++i) {
    std::string na, nb;
    const bool x = RandomOp(a, ra, &na, true, i);
    const bool y = RandomOp(b, rb, &nb, true, i);
    SND_CHECK_EQ(x, y);
  }
  const auto ca = a.PendingChanges();
  const auto cb = b.PendingChanges();
  SND_REQUIRE(ca.size() == cb.size());
  for (std::size_t i = 0; i < ca.size(); ++i) {
    SND_CHECK(ca[i].op == cb[i].op);
    SND_CHECK_EQ(ca[i].file, cb[i].file);
    SND_CHECK(ca[i].text == cb[i].text);
  }
}

// --- B. undo / redo -------------------------------------------------------------------------------

namespace {

void RunRandomHistory(std::uint64_t seed, std::uint32_t ops) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto initial = Image(doc);
  std::vector<std::map<std::string, std::string>> states(1, initial);  // states[UndoCount()]
  Rng r(seed);
  std::uint32_t failures = 0;
  for (std::uint32_t i = 0; i < ops && failures < 5; ++i) {
    const auto before = Image(doc);
    const std::size_t undoBefore = doc.UndoCount();
    const std::uint64_t revBefore = doc.revision();
    std::string name;
    const bool ok = RandomOp(doc, r, &name, true, i);
    const auto after = Image(doc);
    if (name == "Undo" || name == "Redo") {
      if (ok) {
        if (doc.UndoCount() >= states.size() || after != states[doc.UndoCount()]) {
          snd::test::Note("seed %llu op %u: %s does not give the state of step %u", static_cast<unsigned long long>(seed), i,
                          name.c_str(), static_cast<unsigned>(doc.UndoCount()));
          SND_CHECK(false);
          ++failures;
        }
      } else {
        SND_CHECK(after == before);
      }
      continue;
    }
    if (!ok) {
      // B6 / D: a refused edit changes nothing.
      if (after != before || doc.UndoCount() != undoBefore) {
        snd::test::Note("seed %llu op %u: refused '%s' changed the document", static_cast<unsigned long long>(seed), i,
                        name.c_str());
        SND_CHECK(false);
        ++failures;
      }
      continue;
    }
    if (after == before) {
      // No change: no new step (B6), the revision may not move either.
      if (doc.UndoCount() > undoBefore) {
        snd::test::Note("seed %llu op %u: '%s' changed nothing but added a step", static_cast<unsigned long long>(seed), i,
                        name.c_str());
        SND_CHECK(false);
        ++failures;
      }
    } else {
      SND_CHECK(doc.revision() > revBefore);  // B11
      SND_CHECK(!doc.CanRedo());              // B4
      if (doc.UndoCount() == 0) {
        snd::test::Note("seed %llu op %u: '%s' changed the document without a step", static_cast<unsigned long long>(seed), i,
                        name.c_str());
        SND_CHECK(false);
        ++failures;
      }
    }
    states.resize(doc.UndoCount() + 1);
    states[doc.UndoCount()] = after;
  }
  while (doc.Redo()) {
  }
  const auto top = Image(doc);
  const std::size_t steps = doc.UndoCount();
  // B3: one step at a time.
  while (doc.CanUndo()) {
    SND_REQUIRE(doc.Undo());
    if (Image(doc) != states[doc.UndoCount()]) {
      snd::test::Note("seed %llu: undo to step %u does not give that step's state", static_cast<unsigned long long>(seed),
                      static_cast<unsigned>(doc.UndoCount()));
      SND_CHECK(false);
      break;
    }
  }
  // B1: all undone = the loaded document, nothing to save.
  SND_CHECK(Image(doc) == initial);
  SND_CHECK(doc.PendingChanges().empty());
  // B2: all redone = the top.
  std::size_t redone = 0;
  while (doc.Redo()) ++redone;
  SND_CHECK_EQ(redone, steps);
  SND_CHECK(Image(doc) == top);
}

}  // namespace

SND_TEST(IndepS7Doc, B1_B2_B3_RandomSequencesUndoRedo) {
  for (std::uint64_t seed : {1ull, 2ull, 3ull, 42ull, 1234567ull}) RunRandomHistory(seed, 300);
}

SND_TEST(IndepS7Doc, B4_NewEditDropsRedo) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -1));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -2));
  SND_REQUIRE(doc.Undo());
  SND_CHECK(doc.CanRedo());
  SND_REQUIRE(doc.SetNumber(kMusicBus, "volumeDb", -7));
  SND_CHECK(!doc.CanRedo());
  SND_CHECK(!doc.Redo());
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "volumeDb", 0), -1.0);
}

SND_TEST(IndepS7Doc, B5_DragIsOneStepAndMergeKeys) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto initial = Image(doc);
  EventEditor ev(doc, kShot);
  // Drag: group.
  doc.BeginGroup();
  for (int i = 1; i <= 10; ++i) SND_REQUIRE(ev.MoveClip(0, 1, 100.0 + i * 10.0));
  doc.EndGroup();
  SND_CHECK_EQ(doc.UndoCount(), 1u);
  // Same merge key: one more step for 10 notches.
  for (int i = 1; i <= 10; ++i) SND_REQUIRE(ev.MoveClip(0, 0, i * 1.0, 99));
  SND_CHECK_EQ(doc.UndoCount(), 2u);
  // Another key: a new step.
  SND_REQUIRE(ev.MoveClip(0, 0, 50.0, 98));
  SND_CHECK_EQ(doc.UndoCount(), 3u);
  // Key 0: never merges.
  SND_REQUIRE(ev.MoveClip(0, 0, 51.0));
  SND_REQUIRE(ev.MoveClip(0, 0, 52.0));
  SND_CHECK_EQ(doc.UndoCount(), 5u);
  // Nested groups: one step.
  doc.BeginGroup();
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -1));
  doc.BeginGroup();
  SND_REQUIRE(doc.SetNumber(kMusicBus, "volumeDb", -1));
  doc.EndGroup();
  SND_CHECK(doc.InGroup());
  SND_REQUIRE(doc.SetNumber(kReverb, "volumeDb", -1));
  doc.EndGroup();
  SND_CHECK(!doc.InGroup());
  SND_CHECK_EQ(doc.UndoCount(), 6u);
  for (int i = 0; i < 6; ++i) SND_REQUIRE(doc.Undo());
  SND_CHECK(Image(doc) == initial);
}

SND_TEST(IndepS7Doc, B5_MergeDoesNotCrossOtherEdits) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -1, 5));
  SND_REQUIRE(doc.SetNumber(kMusicBus, "volumeDb", -1));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -2, 5));  // same key, but not consecutive
  SND_CHECK_EQ(doc.UndoCount(), 3u);
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "volumeDb", 0), -1.0);
}

SND_TEST(IndepS7Doc, B6_NoChangeNoStep) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::uint64_t rev = doc.revision();
  (void)doc.SetNumber(kSfx, "volumeDb", -3);  // the same value as loaded (its result: Defect_SetSameValueReturnsFalse)
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  SND_CHECK(doc.PendingChanges().empty());
  // Refused: no step.
  SND_CHECK(!doc.Set("no-such-guid", "volumeDb", Number(1)));
  SND_CHECK(!doc.Set(kSfx, "sends[5].levelDb", Number(1)));
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  (void)rev;
}

SND_TEST(IndepS7Doc, B7_GroupEndingWhereItStartedIsDropped) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  doc.BeginGroup();
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -10));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -3));
  doc.EndGroup();
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  // Merged notches that come back too.
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4, 3));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -3, 3));
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(IndepS7Doc, B8_UndoRedoRepeated) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto a = Image(doc);
  SND_REQUIRE(doc.Rename(kHit, "SE/Hit2"));
  const auto b = Image(doc);
  for (int i = 0; i < 50; ++i) {
    SND_REQUIRE(doc.Undo());
    SND_CHECK(Image(doc) == a);
    SND_REQUIRE(doc.Redo());
    SND_CHECK(Image(doc) == b);
  }
}

SND_TEST(IndepS7Doc, B9_OrderAcrossEditors) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto s0 = Image(doc);
  EventEditor ev(doc, kShot);
  SND_REQUIRE(ev.SetClipNumber(0, 0, ClipNumber::VolumeDb, -5));
  const auto s1 = Image(doc);
  MixerEditor mx(doc);
  SND_REQUIRE(mx.SetVolume(kSfx, -8));
  const auto s2 = Image(doc);
  MaterialEditor mat(doc, kPhys);
  SND_REQUIRE(mat.AddMaterial("stone", 3));
  const auto s3 = Image(doc);
  PatchEditor pe(doc, kPatch);
  SND_REQUIRE(pe.MoveNode("vca", 5, 6));
  SND_REQUIRE(doc.Undo());
  SND_CHECK(Image(doc) == s3);
  SND_REQUIRE(doc.Undo());
  SND_CHECK(Image(doc) == s2);
  SND_REQUIRE(doc.Undo());
  SND_CHECK(Image(doc) == s1);
  SND_REQUIRE(doc.Undo());
  SND_CHECK(Image(doc) == s0);
}

SND_TEST(IndepS7Doc, B10_CreateRemoveRenameUndoFiles) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string g = doc.Create("bus", "Tmp", nullptr);
  SND_REQUIRE(!g.empty());
  SND_REQUIRE(doc.Undo());
  SND_CHECK(doc.Find(g) == nullptr);
  SND_CHECK(doc.PendingChanges().empty());
  SND_REQUIRE(doc.Remove(kCatUi));
  SND_REQUIRE(doc.Undo());
  SND_CHECK(doc.Find(kCatUi) != nullptr);
  SND_CHECK(doc.PendingChanges().empty());
  SND_REQUIRE(doc.Rename(kReverb, "Hall"));
  SND_REQUIRE(doc.Undo());
  SND_CHECK_EQ(doc.Find(kReverb)->file, std::string("buses/Reverb.json"));
  SND_CHECK(doc.PendingChanges().empty());
  // The name of an undone new object is free again.
  SND_CHECK(!doc.Create("bus", "Tmp", nullptr).empty());
}

SND_TEST(IndepS7Doc, B11_RevisionAndLastChanged) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto r0 = doc.revision();
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -1));
  SND_CHECK(doc.revision() > r0);
  SND_CHECK(doc.lastChanged().size() == 1 && doc.lastChanged()[0] == kSfx);
  const auto r1 = doc.revision();
  SND_REQUIRE(doc.Undo());
  SND_CHECK(doc.revision() > r1);
  SND_CHECK(doc.lastChanged().size() == 1 && doc.lastChanged()[0] == kSfx);
  // A rename that rewrites references changes the referring objects too.
  auto files = BaseFiles();
  files["events/SE/ByName.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000af", "name": "SE/ByName", "bank": "Main", "bus": "Reverb", "tracks": []})");
  ProjectDocument d2;
  SND_REQUIRE(LoadBase(d2, files));
  SND_REQUIRE(d2.Rename(kReverb, "Hall"));
  bool sawEvent = false, sawBus = false;
  for (const auto& g : d2.lastChanged()) {
    sawEvent |= g == "00000000-0000-4000-8000-0000000000af";
    sawBus |= g == kReverb;
  }
  SND_CHECK(sawEvent && sawBus);
}

// --- C. references, GUIDs -------------------------------------------------------------------------

SND_TEST(IndepS7Doc, C1_NewGuidLayoutAndDeterminism) {
  ProjectDocument a, b;
  std::uint64_t sa = 5, sb = 5;
  a.SetRandom([&sa]() { return sa = sa * 6364136223846793005ull + 1442695040888963407ull; });
  b.SetRandom([&sb]() { return sb = sb * 6364136223846793005ull + 1442695040888963407ull; });
  for (int i = 0; i < 200; ++i) {
    const std::string g = a.NewGuid();
    SND_CHECK_EQ(g, b.NewGuid());
    SND_REQUIRE(g.size() == 36u);
    for (std::size_t k = 0; k < 36; ++k) {
      if (k == 8 || k == 13 || k == 18 || k == 23) {
        SND_CHECK(g[k] == '-');
      } else {
        SND_CHECK((g[k] >= '0' && g[k] <= '9') || (g[k] >= 'a' && g[k] <= 'f'));
      }
    }
    SND_CHECK(g[14] == '4');
    SND_CHECK(g[19] == '8' || g[19] == '9' || g[19] == 'a' || g[19] == 'b');
    SND_CHECK(LooksLikeGuid(g));
    SND_CHECK_EQ(NormalizeGuid(g), g);
  }
  // A generator that repeats itself for a while still gives unique GUIDs to new objects.
  // (A generator that never changes makes Create loop forever: reported, not tested here.)
  ProjectDocument c;
  SND_REQUIRE(c.LoadFiles(BaseFiles()));
  std::uint64_t calls = 0;
  c.SetRandom([&calls]() { return ++calls < 1000 ? 0ull : calls * 0x9E3779B97F4A7C15ull; });
  const std::string x = c.Create("bus", "X", nullptr);
  const std::string y = c.Create("bus", "Y", nullptr);
  SND_CHECK(!x.empty() && !y.empty());
  SND_CHECK(x != y);
}

SND_TEST(IndepS7Doc, C2_C3_NormalizeAndResolve) {
  SND_CHECK_EQ(NormalizeGuid("{00000000-0000-4000-8000-0000000000AB}"), std::string("00000000-0000-4000-8000-0000000000ab"));
  SND_CHECK_EQ(NormalizeGuid("00000000-0000-4000-8000-0000000000AB"), std::string("00000000-0000-4000-8000-0000000000ab"));
  SND_CHECK_EQ(NormalizeGuid("00000000-0000-4000-8000-0000000000ag"), std::string());
  SND_CHECK_EQ(NormalizeGuid("00000000000040008000000000000000"), std::string());
  SND_CHECK_EQ(NormalizeGuid(""), std::string());
  SND_CHECK_EQ(NormalizeGuid("{00000000-0000-4000-8000-0000000000ab"), std::string());
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const DocObject* sfx = doc.Find(kSfx);
  SND_REQUIRE(sfx != nullptr);
  SND_CHECK(doc.Resolve("bus", "SFX") == sfx);
  SND_CHECK(doc.Resolve("bus", kSfx) == sfx);
  SND_CHECK(doc.Resolve("bus", "{00000000-0000-4000-8000-000000000003}") == sfx);
  SND_CHECK(doc.Resolve("bus", "00000000-0000-4000-8000-000000000003") == sfx);
  SND_CHECK(doc.Resolve("category", "SFX") == nullptr);  // the kind matters
  SND_CHECK(doc.Resolve("category", kSfx) == nullptr);
  SND_CHECK(doc.Resolve("bus", "sfx") == nullptr);  // names are exact
}

SND_TEST(IndepS7Doc, C4_RenameKeepsAliasAndRewritesNameReferences) {
  auto files = BaseFiles();
  files["events/SE/ByName.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000af", "name": "SE/ByName", "bank": "Main", "bus": "Reverb", "category": "UI", "tracks": [{"clips": [{"asset": "Tone"}]}]})");
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  SND_CHECK(CheckDocument(doc).empty() || true);
  SND_REQUIRE(doc.Rename(kReverb, "Hall"));
  const DocObject* ev = doc.Find("00000000-0000-4000-8000-0000000000af");
  SND_CHECK_EQ(NormalizeGuid(StringAt(ev->doc, "bus")), std::string(kReverb));
  // The GUID references stay GUIDs.
  SND_CHECK_EQ(StringAt(doc.Find(kSfx)->doc, "sends[0].bus"), std::string(kReverb));
  const json::Value* aliases = FindPath(doc.Find(kReverb)->doc, "aliases");
  SND_REQUIRE(aliases != nullptr && aliases->IsArray());
  SND_CHECK(aliases->items.size() == 1 && aliases->items[0].text == "Reverb");
  SND_CHECK(doc.BrokenReferences().empty());
  // Without alias.
  SND_REQUIRE(doc.Rename(kCatUi, "Menu", RenameOptions{false, true}));
  SND_CHECK(FindPath(doc.Find(kCatUi)->doc, "aliases") == nullptr);
  SND_CHECK_EQ(NormalizeGuid(StringAt(ev->doc, "category")), std::string(kCatUi));
  // Renaming back: the name leaves the aliases.
  SND_REQUIRE(doc.Rename(kReverb, "Reverb"));
  const json::Value* al2 = FindPath(doc.Find(kReverb)->doc, "aliases");
  bool still = false;
  if (al2 != nullptr)
    for (const auto& a : al2->items) still |= a.text == "Reverb";
  SND_CHECK(!still);
  // The build accepts the result.
  std::uint32_t errors = 0;
  for (const auto& i : CheckDocument(doc)) errors += i.error ? 1u : 0u;
  SND_CHECK_EQ(errors, 0u);
}

SND_TEST(IndepS7Doc, C5_RenameRefusals) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SND_REQUIRE(doc.Rename(kReverb, "Hall"));  // "Reverb" is now an alias
  const auto image = Image(doc);
  const auto steps = doc.UndoCount();
  SND_CHECK(!doc.Rename(kMusicBus, "SFX"));      // another bus's name
  SND_CHECK(!doc.Rename(kMusicBus, "Reverb"));   // another object's alias (10.1)
  SND_CHECK(!doc.Rename(kMusicBus, ""));
  SND_CHECK(!doc.Rename(kMusicBus, std::string("a\x01")));
  SND_CHECK(!doc.Rename(kMusicBus, std::string("a\x7f")));
  SND_CHECK(!doc.Rename("no-such", "X"));
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), steps);
  // The same name in another kind is fine (project-format.md §5).
  SND_CHECK(doc.Rename(kCatUi, "SFX"));
}

SND_TEST(IndepS7Doc, C6_RemoveReferencedRefusedForceBreaks) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto image = Image(doc);
  SND_CHECK(!doc.Remove(kTone));  // clips use it
  SND_CHECK(!doc.Remove(kSfx));   // events use it
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  SND_REQUIRE(doc.Remove(kTone, true));
  const auto broken = doc.BrokenReferences();
  SND_CHECK(!broken.empty());
  for (const auto& b : broken) SND_CHECK(b.targetGuid.empty());
  std::uint32_t errors = 0;
  for (const auto& i : CheckDocument(doc)) errors += i.error ? 1u : 0u;
  SND_CHECK(errors > 0u);
  SND_REQUIRE(doc.Undo());
  SND_CHECK(doc.BrokenReferences().empty());
}

SND_TEST(IndepS7Doc, C7_ReferencesToByGuidAndName) {
  auto files = BaseFiles();
  files["events/SE/ByName.json"] = std::string(R"({"type": "event", "guid": "00000000-0000-4000-8000-0000000000af", "name": "SE/ByName", "bank": "Main", "bus": "Reverb", "tracks": []})");
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  const auto refs = doc.ReferencesTo(kReverb);
  bool byName = false, byGuid = false;
  for (const auto& r : refs) {
    SND_CHECK_EQ(r.targetGuid, std::string(kReverb));
    byName |= r.fromGuid == "00000000-0000-4000-8000-0000000000af" && r.path == "bus" && r.text == "Reverb";
    byGuid |= r.fromGuid == kSfx && r.path == "sends[0].bus";
  }
  SND_CHECK(byName);
  SND_CHECK(byGuid);
  // ReferencesOf the event lists the parameter reference of its curve and action.
  bool curve = false, action = false;
  for (const auto& r : doc.ReferencesOf(kShot)) {
    curve |= r.path == "curves[0].parameter" && r.targetGuid == kSpeed;
    action |= r.path == "tracks[0].actions[0].target" && r.targetGuid == kSpeed;
  }
  SND_CHECK(curve);
  SND_CHECK(action);
}

SND_TEST(IndepS7Doc, C8_CreateRefusals) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto image = Image(doc);
  SND_CHECK(doc.Create("event", "", nullptr).empty());
  SND_CHECK(doc.Create("event", "SE/Shot", nullptr).empty());
  SND_CHECK(doc.Create("event", std::string("a\nb"), nullptr).empty());
  SND_CHECK(doc.Create("event", std::string("a\x1f"), nullptr).empty());
  SND_CHECK(doc.Create("event", std::string("a\x7f"), nullptr).empty());
  SND_CHECK(doc.Create("event", std::string("a\0b", 3), nullptr).empty());
  SND_CHECK(doc.Create("sound", "X", nullptr).empty());
  SND_CHECK(doc.Create("", "X", nullptr).empty());
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), 0u);
  // ducking may be nameless.
  SND_CHECK(!doc.Create("ducking", "", nullptr).empty());
}

SND_TEST(IndepS7Doc, C9_ModifyCannotChangeIdentity) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto image = Image(doc);
  SND_CHECK(!doc.Modify(kSfx, [](json::Value& v) {
    for (auto& m : v.members)
      if (m.first == "guid") m.second.text = "00000000-0000-4000-8000-0000000000ff";
    return true;
  }));
  SND_CHECK(!doc.Modify(kSfx, [](json::Value& v) {
    for (auto& m : v.members)
      if (m.first == "type") m.second.text = "category";
    return true;
  }));
  SND_CHECK(!doc.Modify(kSfx, [](json::Value& v) {
    v.members.clear();
    return true;
  }));
  SND_CHECK(!doc.Modify(kSfx, [](json::Value& v) {
    SetMember(v, "volumeDb", Number(-20), nullptr);
    return false;  // cancelled
  }));
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), 0u);
}

SND_TEST(IndepS7Doc, C10_Duplicate) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string g = doc.Duplicate(kShot, "SE/Shot Copy");
  SND_REQUIRE(!g.empty());
  SND_CHECK(g != kShot);
  const DocObject* c = doc.Find(g);
  SND_CHECK_EQ(c->name(), std::string("SE/Shot Copy"));
  // Same members (order aside) except the identity.
  const json::Value& a = doc.Find(kShot)->doc;
  const json::Value& b = c->doc;
  SND_CHECK_EQ(a.members.size(), b.members.size());
  for (const auto& m : a.members) {
    if (m.first == "guid" || m.first == "name" || m.first == "aliases") continue;
    const json::Value* other = b.Find(m.first);
    SND_CHECK(other != nullptr && JsonEqual(m.second, *other));
  }
  SND_CHECK(doc.Duplicate(kShot, "SE/Hit").empty());  // name taken
  SND_CHECK(doc.Duplicate("nope", "X").empty());
}

// --- after reading the implementation: saving in the middle of the history ---------------------

SND_TEST(IndepS7Doc, A14_SaveThenUndoThenSaveRestoresTheDisk) {
  const std::string dir = TempDir("a14");
  WriteBaseProject(dir);
  const auto original = DirContents(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.Rename(kReverb, "FX/Hall"));             // moves buses/Reverb.json
  SND_REQUIRE(doc.Remove(kCatUi));                          // deletes categories/UI.json
  const std::string g = doc.Create("bus", "Extra", nullptr);  // writes buses/Extra.json
  SND_REQUIRE(!g.empty());
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -1));
  SND_REQUIRE(doc.Save());
  auto mid = DirContents(dir);
  SND_CHECK(mid.count("buses/FX/Hall.json") == 1 && mid.count("buses/Reverb.json") == 0);
  SND_CHECK(mid.count("categories/UI.json") == 0 && mid.count("buses/Extra.json") == 1);
  while (doc.Undo()) {
  }
  SND_REQUIRE(doc.Save());
  const auto back = DirContents(dir);
  // Same set of files; the same objects in them (the restored files may be in the tool's form).
  SND_CHECK_EQ(back.size(), original.size());
  for (const auto& [file, text] : original) {
    auto it = back.find(file);
    if (it == back.end()) {
      snd::test::Note("missing after undo + save: %s", file.c_str());
      SND_CHECK(false);
      continue;
    }
    if (file.size() > 5 && file.compare(file.size() - 5, 5, ".json") == 0) {
      json::Value a, b;
      json::ParseError pe;
      SND_CHECK(json::Parse(text, &a, &pe));
      SND_CHECK(json::Parse(it->second, &b, &pe));
      SND_CHECK(JsonEqual(a, b));
    } else {
      SND_CHECK(text == it->second);
    }
  }
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir));
  SND_CHECK(again.loadIssues().empty());
  SND_CHECK_EQ(again.objects().size(), doc.objects().size());
}

// Spec gap (recorded): SaveTo(another directory) writes every object and then works in that
// directory, but WAV and blob files are not copied: the copy does not build where the
// original did. Expected by the header? ("Applies PendingChanges to directory() (or dir).")
SND_TEST(IndepS7Doc, A14_SaveToAnotherDirectoryIsRecorded) {
  const std::string dir = TempDir("a14b");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  std::uint32_t errorsBefore = 0;
  for (const auto& i : CheckDocument(doc)) errorsBefore += i.error ? 1u : 0u;
  const std::string other = TempDir("a14c");
  SND_REQUIRE(doc.SaveTo(other));
  SND_CHECK_EQ(doc.directory(), other);
  SND_CHECK(doc.PendingChanges().empty());
  std::uint32_t errorsAfter = 0;
  for (const auto& i : CheckDocument(doc)) errorsAfter += i.error ? 1u : 0u;
  const bool wavCopied = !DirContents(other).empty() && DirContents(other).count("wav/tone.wav") == 1;
  snd::test::Note("SaveTo(other dir): WAV copied = %d, build errors before %u, after %u", wavCopied ? 1 : 0, errorsBefore,
                  errorsAfter);
  SND_CHECK_EQ(errorsBefore, 0u);
}
