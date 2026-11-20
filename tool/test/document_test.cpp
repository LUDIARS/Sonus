// The editable project (document.hpp) and its build (doc_build.hpp): saving rules,
// unknown members and notes, undo / redo, GUIDs, file names, references, rename and
// aliases, removal, the build's problems tied to members, in-memory banks, the field
// table and the live delta (value vs structure).
#include <cmath>
#include <cstring>

#include "doc_fixture.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/project.hpp"

using namespace snd::tool;
using namespace doctest;
namespace bf = snd::bank_format;

namespace {

std::string Text(const ProjectDocument& d, const std::string& guid) { return d.TextOf(*d.Find(guid)); }

}  // namespace

SND_TEST(Document, NumberFormatRoundTrips) {
  const double values[] = {0.0, 1.0, -3.0, 0.1, -0.25, 1e-7, 123456.789, 3.0000000000000004, 1e21, -144.0, 0.7071067811865476};
  for (double v : values) {
    const std::string t = FormatNumber(v);
    SND_CHECK_MSG(std::strtod(t.c_str(), nullptr) == v, "%s", t.c_str());
    json::Value parsed;
    json::ParseError e;
    SND_CHECK_MSG(json::Parse(t, &parsed, &e), "%s does not parse", t.c_str());
  }
  SND_CHECK_EQ(FormatNumber(2.0), std::string("2"));
  SND_CHECK_EQ(FormatNumber(0.1), std::string("0.1"));
  SND_CHECK_EQ(FormatNumber(NAN), std::string("0"));
  // float32 values survive (the bank stores float).
  const float f = 0.3f;
  SND_CHECK(static_cast<float>(std::strtod(FormatNumber(f).c_str(), nullptr)) == f);
}

SND_TEST(Document, LoadSaveWritesNothingAndKeepsBytes) {
  TempDir dir("noop");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  SND_CHECK_EQ(doc.objects().size(), BaseFiles().size());
  SND_CHECK(doc.loadIssues().empty());
  SND_CHECK(doc.PendingChanges().empty());
  const auto before = dir.Stamps();
  SND_REQUIRE(doc.Save());
  SND_CHECK(dir.Stamps() == before);
  for (const auto& [f, t] : BaseFiles()) SND_CHECK(dir.Read(f) == t);
}

SND_TEST(Document, EditUndoSaveWritesNothing) {
  TempDir dir("undo-noop");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  SND_REQUIRE(doc.SetNumber(G(3), "volumeDb", -9));
  SND_REQUIRE(doc.Set(G(8), "tracks[0].clips[0].startMs", Number(250)));
  SND_CHECK_EQ(doc.PendingChanges().size(), std::size_t{2});
  SND_REQUIRE(doc.Undo());
  SND_REQUIRE(doc.Undo());
  SND_CHECK(doc.PendingChanges().empty());
  const auto before = dir.Stamps();
  SND_REQUIRE(doc.Save());
  SND_CHECK(dir.Stamps() == before);
}

SND_TEST(Document, UnknownMembersAndNotesSurviveEditAndSave) {
  TempDir dir("unknown");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  SND_REQUIRE(doc.SetNumber(G(8), "tracks[0].clips[0].volumeDb", -6));
  SND_REQUIRE(doc.SetNumber(G(2), "volumeDb", -2));
  SND_REQUIRE(doc.Save());
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir.path().string()));
  const json::Value& ev = again.Find(G(8))->doc;
  SND_CHECK_EQ(NumberAt(ev, "x-top", 0), 7.0);
  SND_CHECK(FindPath(ev, "tracks[0].clips[0].x-clip[1]") != nullptr);
  SND_CHECK_EQ(StringAt(ev, "tracks[0].clips[0].notes"), std::string("c"));
  SND_CHECK_EQ(NumberAt(ev, "tracks[0].clips[0].volumeDb", 0), -6.0);
  const json::Value& m = again.Find(G(2))->doc;
  SND_CHECK_EQ(StringAt(m, "x-editor.color"), std::string("#ff0000"));
  SND_CHECK_EQ(StringAt(m, "notes.who"), std::string("mixer"));
  // The member the tool added sits at its spec place (volumeDb after master, before the
  // unknown member that followed master in the file).
  SND_CHECK_EQ(m.members[3].first, std::string("master"));
  SND_CHECK_EQ(m.members[4].first, std::string("volumeDb"));
  SND_CHECK_EQ(m.members[5].first, std::string("x-editor"));
  // Re-saving the reloaded project writes nothing; the written form is json::Write.
  SND_CHECK(again.PendingChanges().empty());
  SND_CHECK(dir.Read("events/Shot.json") == json::Write(ev));
}

SND_TEST(Document, SpecOrderOfNewMembers) {
  ProjectDocument doc;
  LoadBase(doc);
  SND_REQUIRE(doc.Set(G(8), "priority", Number(200)));     // between category and is3D
  SND_REQUIRE(doc.Set(G(8), "fadeOutMs", Number(30)));     // after maxDistance? before it (spec: fadeOutMs < maxDistance)
  SND_REQUIRE(doc.Set(G(8), "tracks[0].clips[0].random.pitchCents", Number(50)));
  SND_REQUIRE(doc.Set(G(8), "tracks[0].clips[0].random.volumeDb", Number(2)));
  const json::Value& e = doc.Find(G(8))->doc;
  std::vector<std::string> keys;
  for (const json::Member& m : e.members) keys.push_back(m.first);
  auto pos = [&](const char* k) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == k) return static_cast<int>(i);
    }
    return -1;
  };
  SND_CHECK(pos("category") < pos("priority") && pos("priority") < pos("is3D"));
  SND_CHECK(pos("fadeOutMs") < pos("maxDistance"));
  const json::Value* r = FindPath(e, "tracks[0].clips[0].random");
  SND_REQUIRE(r != nullptr);
  SND_CHECK_EQ(r->members[0].first, std::string("volumeDb"));
  SND_CHECK_EQ(r->members[1].first, std::string("pitchCents"));
  // random sits after volumeDb in the clip (spec: ... volumeDb, pitchCents, random ...).
  const json::Value* c = FindPath(e, "tracks[0].clips[0]");
  SND_CHECK_EQ(c->members[1].first, std::string("volumeDb"));
  SND_CHECK_EQ(c->members[2].first, std::string("random"));
}

SND_TEST(Document, UndoRedoGroupsAndMerge) {
  ProjectDocument doc;
  LoadBase(doc);
  const std::string start = Text(doc, G(3));
  // A drag: one step.
  doc.BeginGroup();
  for (int i = 1; i <= 10; ++i) doc.SetNumber(G(3), "volumeDb", -i);
  doc.EndGroup();
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{1});
  // Wheel notches with one merge key: one step; a different key: another.
  doc.SetNumber(G(3), "lodPriority", 1, 77);
  doc.SetNumber(G(3), "lodPriority", 2, 77);
  doc.SetNumber(G(3), "lodPriority", 3, 77);
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{2});
  doc.SetNumber(G(2), "volumeDb", -1, 78);
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{3});
  // A group that ends where it started is no step.
  doc.BeginGroup();
  doc.SetNumber(G(4), "volumeDb", -5);
  doc.Erase(G(4), "volumeDb");
  doc.EndGroup();
  SND_CHECK_EQ(doc.UndoCount(), std::size_t{3});
  SND_CHECK(doc.Undo());
  SND_CHECK_EQ(NumberAt(doc.Find(G(2))->doc, "volumeDb", 99), 99.0);
  SND_CHECK(doc.Undo());
  SND_CHECK_EQ(NumberAt(doc.Find(G(3))->doc, "lodPriority", 0), 0.0);
  SND_CHECK(doc.Undo());
  SND_CHECK(Text(doc, G(3)) == start);
  SND_CHECK(!doc.Undo());
  SND_CHECK(doc.Redo());
  SND_CHECK_EQ(NumberAt(doc.Find(G(3))->doc, "volumeDb", 0), -10.0);
  SND_CHECK(doc.Redo());
  SND_CHECK_EQ(NumberAt(doc.Find(G(3))->doc, "lodPriority", 0), 3.0);
  // A new edit clears the redo list.
  doc.SetNumber(G(5), "volumeDb", -2);
  SND_CHECK(!doc.CanRedo());
  // Everything back: the document equals the loaded one.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.PendingChanges().empty());
  // No change, no step.
  const std::size_t n = doc.UndoCount();
  SND_CHECK(!doc.SetNumber(G(3), "volumeDb", -1.5));
  SND_CHECK_EQ(doc.UndoCount(), n);
}

SND_TEST(Document, CreateUsesGuidSourceAndFileRules) {
  ProjectDocument doc;
  LoadBase(doc);
  doc.SetRandom([] { return 0x0123456789ABCDEFull; });
  const std::string g = doc.Create("event", "SE/Foot:Step");
  SND_REQUIRE(!g.empty());
  SND_CHECK_EQ(g, std::string("01234567-89ab-4def-8123-456789abcdef"));
  SND_CHECK(LooksLikeGuid(g));
  const DocObject* o = doc.Find(g);
  SND_CHECK_EQ(o->file, std::string("events/SE/Foot_Step.json"));
  SND_CHECK_EQ(o->doc.members[0].first, std::string("type"));
  SND_CHECK_EQ(o->doc.members[1].first, std::string("guid"));
  SND_CHECK_EQ(o->doc.members[2].first, std::string("name"));
  // Same generator output again: the GUID is taken, the next one is drawn.
  std::uint64_t k = 0;
  doc.SetRandom([&k] { return k++ < 2 ? 0x0123456789ABCDEFull : 0x42ull * k; });
  const std::string g2 = doc.Create("event", "SE/Foot_Step");
  SND_CHECK(!g2.empty() && g2 != g);
  SND_CHECK_EQ(doc.Find(g2)->file, std::string("events/SE/Foot_Step_2.json"));  // clash -> _2
  // Hidden parts and odd names.
  const std::string g3 = doc.Create("bus", ".hidden/x?");
  SND_CHECK_EQ(doc.Find(g3)->file, std::string("buses/_.hidden/x_.json"));
  // Refused: unknown kind, empty name, taken name, control characters, a second project.
  SND_CHECK(doc.Create("evnt", "A").empty());
  SND_CHECK(doc.Create("event", "").empty());
  SND_CHECK(doc.Create("event", "Shot").empty());
  SND_CHECK(doc.Create("event", std::string("a\x01")).empty());
  SND_CHECK(doc.Create("project", "P2").empty());
  SND_CHECK(!doc.Create("ducking", "").empty());  // ducking needs no name
  // Undo removes the new objects.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.Find(g) == nullptr);
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(Document, RenameKeepsAliasRewritesNameReferencesAndMovesFile) {
  TempDir dir("rename");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  // Sfx is referenced by GUID (Shot.bus) and by name (Quiet.values[0].bus); category SE by name.
  SND_REQUIRE(doc.Rename(G(3), "Effects"));
  const json::Value& bus = doc.Find(G(3))->doc;
  SND_CHECK_EQ(StringAt(bus, "name"), std::string("Effects"));
  SND_CHECK_EQ(StringAt(bus, "aliases[0]"), std::string("Sfx"));
  SND_CHECK_EQ(doc.Find(G(3))->file, std::string("buses/Effects.json"));
  SND_CHECK_EQ(StringAt(doc.Find(G(9))->doc, "values[0].bus"), G(3));
  SND_CHECK_EQ(StringAt(doc.Find(G(8))->doc, "bus"), G(3));
  SND_CHECK(doc.BrokenReferences().empty());
  // The build accepts it, and keeps the old name as an alias.
  const DocBuild b = BuildDocument(doc);
  SND_CHECK_MSG(b.ok, "%s", Dump(b.issues).c_str());
  // Renaming back: "Sfx" is no longer an alias ("Effects" becomes one); the file returns.
  SND_REQUIRE(doc.Rename(G(3), "Sfx"));
  SND_CHECK_EQ(FindPath(doc.Find(G(3))->doc, "aliases")->items.size(), std::size_t{1});
  SND_CHECK_EQ(StringAt(doc.Find(G(3))->doc, "aliases[0]"), std::string("Effects"));
  SND_CHECK_EQ(doc.Find(G(3))->file, std::string("buses/Sfx.json"));
  SND_CHECK(!doc.Rename(G(3), "Reverb"));   // another bus's name
  SND_CHECK(!doc.Rename(G(4), "Effects"));  // an old name of another object (10.1)
  // Without keeping the alias.
  SND_REQUIRE(doc.Rename(G(3), "Fx", RenameOptions{false, true}));
  SND_CHECK_EQ(FindPath(doc.Find(G(3))->doc, "aliases")->items.size(), std::size_t{1});
  SND_REQUIRE(doc.Save());
  SND_CHECK(!dir.Exists("buses/Sfx.json"));
  SND_CHECK(dir.Exists("buses/Fx.json"));
  // Undo of a rename puts the file back.
  SND_REQUIRE(doc.Rename(G(5), "UI", RenameOptions{true, true}));
  SND_CHECK_EQ(doc.Find(G(5))->file, std::string("categories/UI.json"));
  doc.Undo();
  SND_CHECK_EQ(doc.Find(G(5))->file, std::string("categories/SE.json"));
}

SND_TEST(Document, RenameLeavesFilesOutsideTheNamingRule) {
  std::map<std::string, std::string> f = BaseFiles();
  f.erase("categories/SE.json");
  f["misc/se-file.json"] = "{\"type\": \"category\", \"guid\": \"" + G(5) + "\", \"name\": \"SE\"}\n";
  ProjectDocument doc;
  LoadBase(doc, f);
  SND_REQUIRE(doc.Rename(G(5), "UI"));
  SND_CHECK_EQ(doc.Find(G(5))->file, std::string("misc/se-file.json"));
  SND_REQUIRE(doc.Rename(G(5), "UI2", RenameOptions{true, false}));
  SND_CHECK_EQ(doc.Find(G(5))->file, std::string("misc/se-file.json"));
}

SND_TEST(Document, ReferencesRemoveAndBrokenReferences) {
  TempDir dir("remove");
  WriteAll(dir, BaseFiles());
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  const std::vector<Reference> toSfx = doc.ReferencesTo(G(3));
  // Shot.bus (GUID), Quiet.values[0].bus (name).
  SND_CHECK_EQ(toSfx.size(), std::size_t{2});
  const std::vector<Reference> ofShot = doc.ReferencesOf(G(8));
  bool sawCurve = false, sawBank = false;
  for (const Reference& r : ofShot) {
    sawCurve = sawCurve || (r.path == "curves[0].parameter" && r.targetGuid == G(6));
    sawBank = sawBank || (r.path == "bank" && r.targetGuid == G(1));
  }
  SND_CHECK(sawCurve && sawBank);
  // Referenced: refused; forced: removed, references broken, file deleted on save.
  SND_CHECK(!doc.Remove(G(7)));
  SND_REQUIRE(doc.Remove(G(7), true));
  const std::vector<Reference> broken = doc.BrokenReferences();
  SND_REQUIRE_EQ(broken.size(), std::size_t{1});
  SND_CHECK_EQ(broken[0].path, std::string("tracks[0].clips[0].asset"));
  SND_CHECK_EQ(broken[0].targetKind, std::string("asset"));
  // The build reports it at the member.
  const std::vector<DocIssue> issues = CheckDocument(doc);
  bool found = false;
  for (const DocIssue& i : issues) found = found || (i.guid == G(8) && i.path == "tracks[0].clips[0].asset" && i.error);
  SND_CHECK_MSG(found, "%s", Dump(issues).c_str());
  SND_REQUIRE(doc.Save());
  SND_CHECK(!dir.Exists("assets/tone.json"));
  // Unreferenced objects go without force.
  SND_CHECK(doc.Remove(G(9)));
}

SND_TEST(Document, GuardsAgainstBadEdits) {
  ProjectDocument doc;
  LoadBase(doc);
  SND_CHECK(!doc.Set(G(8), "guid", String(G(77))));
  SND_CHECK(!doc.Set(G(8), "type", String("bus")));
  SND_CHECK(!doc.Erase(G(8), "name"));
  SND_CHECK(!doc.Set(G(8), "tracks[5].flags", Number(1)));        // no such element
  SND_CHECK(!doc.Set(G(8), "tracks[0", Number(1)));               // malformed path
  SND_CHECK(!doc.Set("not-a-guid", "x", Number(1)));
  SND_CHECK(!doc.Modify(G(8), [](json::Value& v) {
    SetMember(v, "type", String("bus"), nullptr);
    return true;
  }));
  SND_CHECK(!doc.Modify(G(8), [](json::Value& v) {
    SetMember(v, "name", String("Shot"), nullptr);  // unchanged name is fine...
    SetMember(v, "guid", String(G(55)), nullptr);    // ...a new guid is not
    return true;
  }));
  SND_CHECK(doc.UndoCount() == 0u);
  SND_CHECK(doc.PendingChanges().empty());
  // Arrays: append, insert, move.
  SND_CHECK(doc.Set(G(8), "tracks[1]", json::MakeObject()));
  SND_CHECK(doc.Insert(G(8), "tracks", 0, json::MakeObject()));
  SND_CHECK_EQ(FindPath(doc.Find(G(8))->doc, "tracks")->items.size(), std::size_t{3});
  SND_CHECK(doc.MoveElement(G(8), "tracks", 1, 0));
  SND_CHECK(FindPath(doc.Find(G(8))->doc, "tracks[0].clips") != nullptr);
  SND_CHECK(!doc.MoveElement(G(8), "tracks", 0, 9));
}

SND_TEST(Document, BrokenInputDoesNotCrash) {
  std::map<std::string, std::string> f = BaseFiles();
  f["events/bad.json"] = "{\"type\": \"event\", \"guid\": ";                       // truncated
  f["events/arr.json"] = "[1, 2, 3]";                                                // not an object
  f["events/noguid.json"] = "{\"type\": \"event\", \"name\": \"N\"}";
  f["events/dup.json"] = "{\"type\": \"event\", \"guid\": \"" + G(8) + "\", \"name\": \"Dup\"}";
  f["events/types.json"] = "{\"type\": \"event\", \"guid\": \"" + G(60) + "\", \"name\": 5, \"priority\": \"high\", "
                           "\"maxDistance\": 1e999, \"retriggerMs\": -1e308, \"tracks\": {\"a\": 1}}";
  f["events/huge.json"] = "{\"type\": \"event\", \"guid\": \"" + G(61) + "\", \"name\": \"Huge\", \"bank\": \"Main\", "
                          "\"priority\": 18446744073709551616, \"maxInstances\": 99999999999999999999999}";
  std::string deep = "{\"type\": \"event\", \"guid\": \"" + G(62) + "\", \"name\": \"Deep\", \"notes\": ";
  for (int i = 0; i < 200; ++i) deep += "[";
  for (int i = 0; i < 200; ++i) deep += "]";
  f["events/deep.json"] = deep + "}";
  ProjectDocument doc;
  LoadBase(doc, f);
  SND_CHECK_EQ(doc.loadIssues().size(), std::size_t{4});
  SND_CHECK_EQ(doc.unreadFiles().size(), std::size_t{4});
  const std::vector<DocIssue> issues = CheckDocument(doc);
  SND_CHECK(!issues.empty());
  SND_CHECK(HasIssue(issues, "\"name\" must be a string"));
  // Edits on odd objects stay safe.
  doc.SetNumber(G(60), "priority", 3);
  doc.Set(G(60), "tracks[0]", Number(1));  // tracks is an object: refused
  SND_CHECK(doc.PendingChanges().size() == 1u);
  Project p;
  p.Load(doc);  // no crash; issues reported
}

SND_TEST(Document, IssuesMatchTheBuildToolWordForWord) {
  TempDir dir("issues");
  std::map<std::string, std::string> f = BaseFiles();
  f["events/Typo.json"] = "{\"type\": \"event\", \"guid\": \"" + G(20) + "\", \"name\": \"Typo\", \"bank\": \"Main\", "
                          "\"prority\": 3, \"tracks\": [{\"clips\": [{\"asset\": \"nope\"}]}]}";
  WriteAll(dir, f);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir.path().string()));
  doc.SetBinaryFile("audio/tone.wav", ToneWav());
  const std::vector<DocIssue> mem = CheckDocument(doc);
  // The same project through the CLI path (directory).
  bt::BuildOptions o;
  o.projectDir = dir.path().string();
  o.outDir = (dir.path() / ".out").string();
  bt::Diagnostics d;
  bt::Build(o, d, nullptr);
  SND_REQUIRE_EQ(mem.size(), d.items().size());
  for (std::size_t i = 0; i < mem.size(); ++i) {
    SND_CHECK_MSG(mem[i].text == bt::Diagnostics::Format(d.items()[i]), "%s vs %s", mem[i].text.c_str(),
                  bt::Diagnostics::Format(d.items()[i]).c_str());
  }
  bool unknownAt = false, refAt = false;
  for (const DocIssue& i : mem) {
    unknownAt = unknownAt || (!i.error && i.guid == G(20) && i.path == "prority");
    refAt = refAt || (i.error && i.guid == G(20) && i.path == "tracks[0].clips[0].asset");
  }
  SND_CHECK_MSG(unknownAt && refAt, "%s", Dump(mem).c_str());
  // After an edit, the problem positions are those of the text Save writes.
  SND_REQUIRE(doc.Set(G(20), "tracks[0].clips[0].asset", String(G(7))));
  SND_REQUIRE(doc.Erase(G(20), "prority"));
  const std::vector<DocIssue> after = CheckDocument(doc);
  SND_CHECK_MSG(IssuesOf(after, G(20)).empty(), "%s", Dump(after).c_str());
}

SND_TEST(Document, BuildInMemoryAndProjectView) {
  ProjectDocument doc;
  LoadBase(doc);
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  SND_REQUIRE_EQ(b.banks.size(), std::size_t{1});
  SND_CHECK(b.banks[0].view() != nullptr);
  SND_CHECK(b.buildHash != 0u);
  bool ids = false;
  for (const bt::OutputFile& f : b.files) ids = ids || f.path == "ids.json";
  SND_CHECK(ids);
  // Deterministic: the same document builds the same bytes.
  const DocBuild b2 = BuildDocument(doc);
  SND_CHECK(b2.banks[0].bytes == b.banks[0].bytes && b2.buildHash == b.buildHash);
  // Also on disk (sonus-build layout).
  TempDir out("out");
  DocBuildOptions o;
  o.outDir = out.path().string();
  SND_CHECK(BuildDocument(doc, o).ok);
  SND_CHECK(out.Exists("Main.sndb") && out.Exists("ids.json"));
  // The read-only Project view from the document (unsaved edits included).
  doc.SetNumber(G(3), "volumeDb", -7);
  Project p;
  SND_REQUIRE(p.Load(doc));
  SND_REQUIRE_EQ(p.buses().size(), std::size_t{3});
  SND_CHECK_EQ(p.buses()[0].name, std::string("Master"));
  const std::int32_t sfx = p.FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(sfx >= 0);
  SND_CHECK_EQ(p.buses()[static_cast<std::size_t>(sfx)].volumeDb, -7.0f);
}

SND_TEST(Document, FieldTableLocatesEveryCell) {
  // Each FieldColumns entry: give the member a distinctive value, build, LocateField finds
  // the cell and the bank holds the value there.
  ProjectDocument doc;
  std::map<std::string, std::string> f = BaseFiles();
  f["voiceLimitGroups/Steps.json"] = "{\"type\": \"voiceLimitGroup\", \"guid\": \"" + G(30) + "\", \"name\": \"Steps\", \"maxVoices\": 4}";
  f["duckings/D1.json"] = "{\"type\": \"ducking\", \"guid\": \"" + G(31) + "\", \"name\": \"D1\", \"bank\": \"Main\", \"kind\": \"sidechain\", "
                          "\"source\": \"Sfx\", \"target\": \"Reverb\", \"amountDb\": -6}";
  f["events/Shot.json"] =
      "{\"type\": \"event\", \"guid\": \"" + G(8) + "\", \"name\": \"Shot\", \"bank\": \"Main\", \"bus\": \"Sfx\", \"category\": \"SE\", "
      "\"limitGroup\": \"Steps\", \"curves\": [{\"parameter\": \"Speed\", \"target\": \"pitch\", \"points\": [[0, 0], [10, 1200, 2]]}], "
      "\"tracks\": [{\"layerParameter\": \"Speed\", \"clips\": [{\"asset\": \"tone\", \"layer\": [0, 5]}], "
      "\"actions\": [{\"timeMs\": 10, \"action\": \"setParameter\", \"target\": \"Speed\", \"value\": 2}]}]}";
  LoadBase(doc, f);
  const std::string ev = G(8), bus = G(3), cat = G(5), prm = G(6), snap = G(9), grp = G(30), duck = G(31);
  const struct {
    std::string guid;
    const char* path;
    double value;
  } cases[] = {
      {ev, "maxInstances", 7},          {ev, "retriggerMs", 33},           {ev, "fadeOutMs", 44},
      {ev, "comboWindowMs", 55},        {ev, "maxDistance", 66},           {ev, "cone.innerDeg", 30},
      {ev, "cone.outerDeg", 90},        {ev, "cone.outerDb", -9},          {ev, "priority", 77},
      {ev, "tracks[0].flags", 1},       {ev, "tracks[0].clips[0].startMs", 12}, {ev, "tracks[0].clips[0].weight", 3},
      {ev, "tracks[0].clips[0].volumeDb", -4}, {ev, "tracks[0].clips[0].pitchCents", 25},
      {ev, "tracks[0].clips[0].random.volumeDb", 2}, {ev, "tracks[0].clips[0].random.pitchCents", 50},
      {ev, "tracks[0].clips[0].random.pan", 0.5}, {ev, "tracks[0].clips[0].random.startMs", 5},
      {ev, "tracks[0].clips[0].random.filterHz", 800}, {ev, "tracks[0].clips[0].switchValue", 3},
      {ev, "tracks[0].clips[0].layer[0]", 1}, {ev, "tracks[0].clips[0].layer[1]", 4},
      {ev, "tracks[0].clips[0].fadeInMs", 6}, {ev, "tracks[0].clips[0].fadeOutMs", 7},
      {ev, "tracks[0].clips[0].loopCount", 2}, {ev, "tracks[0].actions[0].timeMs", 15},
      {ev, "tracks[0].actions[0].value", 4}, {ev, "curves[0].points[1][0]", 9}, {ev, "curves[0].points[1][1]", 900},
      {ev, "curves[0].points[1][2]", 3},
      {bus, "volumeDb", -2.5},           {bus, "channels", 2},              {bus, "lodPriority", 4},
      {bus, "sends[0].levelDb", -20},    {bus, "effects[0].params[0]", 2000},
      {cat, "volumeDb", -3},             {prm, "min", -1},                  {prm, "max", 20},
      {prm, "default", 2},               {prm, "rampRate", 8},              {snap, "values[0].value", -9},
      {grp, "maxVoices", 6},             {duck, "amountDb", -12},           {duck, "attackMs", 3},
      {duck, "releaseMs", 300},          {duck, "thresholdDb", -30},
  };
  for (const auto& c : cases) {
    SND_REQUIRE(doc.SetNumber(c.guid, c.path, c.value));
    const DocBuild b = BuildDocument(doc);
    SND_REQUIRE_MSG(b.ok, "%s: %s", c.path, Dump(b.issues).c_str());
    const std::vector<CellRef> cells = LocateField(b, doc, c.guid, c.path);
    SND_REQUIRE_MSG(cells.size() == 1u, "%s: %zu cells", c.path, cells.size());
    const CellRef& cell = cells[0];
    double got = cell.type == CellType::F32 ? BitsToFloat(cell.bits) : static_cast<double>(cell.bits);
    SND_CHECK_MSG(std::fabs(got - c.value) < 1e-3, "%s: bank holds %g, expected %g", c.path, got, c.value);
    SND_CHECK_MSG(cell.addressable, "%s not addressable", c.path);
    SND_CHECK_MSG(cell.guid.lo != 0u || cell.guid.hi != 0u, "%s: no guid", c.path);
  }
}

SND_TEST(Document, LiveDeltaSeparatesValueAndStructureEdits) {
  ProjectDocument doc;
  LoadBase(doc);
  const DocBuild a = BuildDocument(doc);
  SND_REQUIRE(a.ok);
  // Value edit: one clip volume -> exactly one live cell, addressed like core ResolveRow.
  SND_REQUIRE(doc.SetNumber(G(8), "tracks[0].clips[0].volumeDb", -9));
  const DocBuild b = BuildDocument(doc);
  LiveDelta d = DiffBuilds(a, b);
  SND_CHECK(d.kind == EditClass::Value);
  SND_REQUIRE_EQ(d.values.size(), std::size_t{1});
  const CellRef& c = d.values[0].cell;
  SND_CHECK_EQ(c.chunk, bf::chunk::CLIP);
  SND_CHECK_EQ(c.column, static_cast<std::uint32_t>(bf::col::CLIP_volumeDb));
  SND_CHECK_EQ(c.runtimeId, snd::HashId("Shot"));
  SND_CHECK_EQ(c.index, 0u);
  SND_CHECK(c.liveWritable);
  SND_CHECK_EQ(BitsToFloat(c.bits), -9.0f);
  SND_CHECK_EQ(BitsToFloat(d.values[0].oldBits), -3.0f);
  // The located cell of the same member is the same.
  const std::vector<CellRef> loc = LocateField(b, doc, G(8), "tracks[0].clips[0].volumeDb");
  SND_REQUIRE_EQ(loc.size(), std::size_t{1});
  SND_CHECK(loc[0].row == c.row && loc[0].index == c.index && loc[0].runtimeId == c.runtimeId);
  // Notes and editor-only members do not reach the bank.
  ProjectDocument before = doc;
  SND_REQUIRE(doc.Set(G(8), "notes", String("hello")));
  SND_CHECK(ClassifyEdit(before, doc) == EditClass::None);
  // Effect parameter: EFPV, index effect << 16 | parameter, owner = the bus.
  before = doc;
  SND_REQUIRE(doc.SetNumber(G(3), "effects[0].params[0]", 3000));
  SND_CHECK(ClassifyEdit(before, doc, &d) == EditClass::Value);
  SND_REQUIRE_EQ(d.values.size(), std::size_t{1});
  SND_CHECK(d.values[0].cell.chunk == bf::chunk::EFPV && d.values[0].cell.runtimeId == snd::HashId("Sfx") &&
            d.values[0].cell.index == 0u);
  // Priority is read by game threads: not live (D-10) -> chunk replacement.
  before = doc;
  SND_REQUIRE(doc.SetNumber(G(8), "priority", 10));
  SND_CHECK(ClassifyEdit(before, doc, &d) == EditClass::Structure);
  SND_CHECK(!d.chunks.empty() && d.chunks[0].fourcc == bf::chunk::EVNT);
  // A new clip: rows change -> structure.
  before = doc;
  json::Value clip = json::MakeObject();
  json::Add(clip, "asset", String(G(7)));
  SND_REQUIRE(doc.Insert(G(8), "tracks[0].clips", 1, clip));
  SND_CHECK(ClassifyEdit(before, doc, &d) == EditClass::Structure);
  bool clipChunk = false;
  for (const ChunkEdit& e : d.chunks) clipChunk = clipChunk || e.fourcc == bf::chunk::CLIP;
  SND_CHECK(clipChunk);
  // A rename: names / ids change -> structure.
  before = doc;
  SND_REQUIRE(doc.Rename(G(8), "Shot2"));
  SND_CHECK(ClassifyEdit(before, doc) == EditClass::Structure);
  // A new bank: added.
  before = doc;
  const std::string nb = doc.Create("bank", "Extra");
  json::Value ev = json::MakeObject();
  json::Add(ev, "bank", String(nb));
  SND_REQUIRE(!doc.Create("event", "Other", &ev).empty());
  ClassifyEdit(before, doc, &d);
  SND_CHECK(d.kind == EditClass::Structure && d.banksAdded.size() == 1u && d.banksAdded[0] == "Extra.sndb");
}
