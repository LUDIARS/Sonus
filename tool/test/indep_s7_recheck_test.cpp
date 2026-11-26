// Independent re-check of group M after the decisions D-176..D-180 (properties R* of
// tool/test/indep_s7_model_properties.md, "再確認"). Written by the test owner from the decisions
// (_briefs/M-fix.md) and the changed public headers; implementation-specific failure cases after
// reading the changed code.
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "snd/buildtool/schema.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "timeline.hpp"  // music/src: the runtime's time line (D-179c comparison)

using namespace indep_s7;
using namespace snd::tool;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool AnyTempFile(const std::string& dir) {
  for (const auto& [f, t] : DirContents(dir))
    if (f.find("sonus-tmp") != std::string::npos) return true;
  return false;
}

// Disk and document agree: every object whose file has no pending change is on disk as TextOf.
void CheckDiskMatchesSavedState(const ProjectDocument& doc, const std::string& dir) {
  const auto pending = doc.PendingChanges();
  for (const auto& o : doc.objects()) {
    bool isPending = false;
    for (const auto& c : pending) isPending |= c.file == o.file;
    if (isPending) continue;
    const std::string disk = ReadFileText(dir + "/" + o.file);
    if (disk != doc.TextOf(o)) {
      snd::test::Note("%s: not pending, but the disk differs", o.file.c_str());
      SND_CHECK(false);
    }
  }
}

// Leaf member paths of a JSON object ("tracks[0].clips[1].startMs").
void Leaves(const json::Value& v, const std::string& path, std::vector<std::pair<std::string, const json::Value*>>* out) {
  if (v.IsObject()) {
    for (const auto& m : v.members) Leaves(m.second, path.empty() ? m.first : path + "." + m.first, out);
  } else if (v.IsArray()) {
    for (std::size_t i = 0; i < v.items.size(); ++i) Leaves(v.items[i], path + "[" + std::to_string(i) + "]", out);
  } else {
    out->push_back({path, &v});
  }
}

}  // namespace

// --- R1: saving (D-176d) -------------------------------------------------------------------------------

SND_TEST(IndepS7Recheck, R1_WriteFailureInTheMiddleKeepsDocumentAndDiskConsistent) {
  const std::string dir = TempDir("r1");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.SetNumber(kMaster, "volumeDb", -1));    // buses/Master.json
  SND_REQUIRE(doc.SetNumber(kMusicBus, "volumeDb", -2));  // buses/Music.json  (blocked)
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4));       // buses/SFX.json
  SND_REQUIRE(doc.Remove(kCatUi));                         // categories/UI.json: deletion
  std::error_code ec;
  fs::remove(dir + "/buses/Music.json", ec);
  fs::create_directories(dir + "/buses/Music.json/block", ec);
  std::string err;
  SND_CHECK(!doc.Save(&err));
  SND_CHECK(!err.empty());
  const SaveResult& r = doc.lastSave();
  SND_CHECK(!r.ok);
  SND_REQUIRE(r.failures.size() == 1u);
  SND_CHECK_EQ(r.failures[0].path, std::string("buses/Music.json"));
  SND_CHECK(!r.failures[0].reason.empty());
  SND_CHECK(doc.IsDirty());
  // Only the failed write (and the deletion, which waits for every write) is still pending.
  bool musicPending = false, uiPending = false;
  for (const auto& c : doc.PendingChanges()) {
    musicPending |= c.file == "buses/Music.json" && c.op == FileChange::Op::Write;
    uiPending |= c.file == "categories/UI.json" && c.op == FileChange::Op::Delete;
    SND_CHECK(c.file == "buses/Music.json" || c.file == "categories/UI.json");
  }
  SND_CHECK(musicPending);
  SND_CHECK(uiPending);
  SND_CHECK(fs::exists(dir + "/categories/UI.json"));  // not deleted while a write failed
  CheckDiskMatchesSavedState(doc, dir);
  SND_CHECK(!AnyTempFile(dir));
  // The next Save after the obstacle is gone does the rest.
  fs::remove_all(dir + "/buses/Music.json", ec);
  SND_CHECK(doc.Save(&err));
  SND_CHECK(doc.lastSave().ok && doc.lastSave().failures.empty());
  SND_CHECK(!doc.IsDirty());
  SND_CHECK(!fs::exists(dir + "/categories/UI.json"));
  CheckDiskMatchesSavedState(doc, dir);
  SND_CHECK(!AnyTempFile(dir));
  ProjectDocument again;
  SND_REQUIRE(again.Load(dir));
  SND_CHECK(Image(again) == Image(doc));
}

SND_TEST(IndepS7Recheck, R1_TemporaryPathBlockedFailsCleanly) {
  const std::string dir = TempDir("r1t");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4));
  std::error_code ec;
  // Whatever the temporary name is, a non-empty directory at "<file>.sonus-tmp" or the file being
  // unreplaceable must give a failure, never a half-written file.
  fs::create_directories(dir + "/buses/SFX.json.sonus-tmp/x", ec);
  const std::string before = ReadFileText(dir + "/buses/SFX.json");
  const bool ok = doc.Save();
  if (!ok) {
    SND_CHECK(doc.IsDirty());
    SND_CHECK_EQ(ReadFileText(dir + "/buses/SFX.json"), before);
    SND_REQUIRE(doc.lastSave().failures.size() == 1u);
    SND_CHECK_EQ(doc.lastSave().failures[0].path, std::string("buses/SFX.json"));
  } else {
    CheckDiskMatchesSavedState(doc, dir);
  }
  fs::remove_all(dir + "/buses/SFX.json.sonus-tmp", ec);
  SND_CHECK(doc.Save());
  CheckDiskMatchesSavedState(doc, dir);
  SND_CHECK(!AnyTempFile(dir));
}

SND_TEST(IndepS7Recheck, R1_FailedDeletionIsRetried) {
  const std::string dir = TempDir("r1d");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.Remove(kCatUi));
  std::error_code ec;
  fs::remove(dir + "/categories/UI.json", ec);
  fs::create_directories(dir + "/categories/UI.json/keep", ec);
  SND_CHECK(!doc.Save());
  SND_REQUIRE(doc.lastSave().failures.size() == 1u);
  SND_CHECK_EQ(doc.lastSave().failures[0].path, std::string("categories/UI.json"));
  SND_CHECK(doc.IsDirty());
  // The file name stays reserved: a new object does not get it while the deletion is pending.
  const std::string g = doc.Create("category", "UI", nullptr);
  SND_REQUIRE(!g.empty());
  SND_CHECK(doc.Find(g)->file != "categories/UI.json");
  SND_REQUIRE(doc.Undo());
  fs::remove_all(dir + "/categories/UI.json", ec);
  WriteFile(dir + "/categories/UI.json", "{}");  // a plain file again: deletable
  SND_CHECK(doc.Save());
  SND_CHECK(!fs::exists(dir + "/categories/UI.json"));
  SND_CHECK(!doc.IsDirty());
}

SND_TEST(IndepS7Recheck, R1_MovedFileKeepsTheOldOneUntilTheNewOneIsWritten) {
  const std::string dir = TempDir("r1m");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.Rename(kReverb, "FX/Hall"));
  std::error_code ec;
  fs::create_directories(dir + "/buses/FX/Hall.json/block", ec);
  SND_CHECK(!doc.Save());
  SND_CHECK(fs::exists(dir + "/buses/Reverb.json"));  // the old file stays
  ProjectDocument disk;
  SND_REQUIRE(disk.Load(dir));
  SND_CHECK(disk.Find(kReverb) != nullptr);  // the object is on disk exactly once
  for (const auto& i : disk.loadIssues()) SND_CHECK(i.message.find("duplicate guid") == std::string::npos);
  fs::remove_all(dir + "/buses/FX/Hall.json", ec);
  SND_CHECK(doc.Save());
  SND_CHECK(!fs::exists(dir + "/buses/Reverb.json"));
  SND_CHECK(fs::exists(dir + "/buses/FX/Hall.json"));
  ProjectDocument after;
  SND_REQUIRE(after.Load(dir));
  SND_CHECK(after.loadIssues().empty());
  SND_CHECK_EQ(after.Find(kReverb)->name(), std::string("FX/Hall"));
}

SND_TEST(IndepS7Recheck, R1_SuccessfulSaveResult) {
  const std::string dir = TempDir("r1s");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4));
  SND_CHECK(doc.Save());
  SND_CHECK(doc.lastSave().ok);
  SND_CHECK(doc.lastSave().failures.empty());
  SND_CHECK(!AnyTempFile(dir));
  // Nothing to save: still ok.
  SND_CHECK(doc.Save());
  SND_CHECK(doc.lastSave().ok);
}

// --- R2: save as (D-176e) ------------------------------------------------------------------------------

SND_TEST(IndepS7Recheck, R2_SaveAsOutsideMissingAndOverwrite) {
  const std::string root = TempDir("r2");
  const std::string dir = root + "/proj";
  auto files = BaseFiles();
  files["assets/Out.json"] = R"({"type": "asset", "guid": "00000000-0000-4000-8000-0000000000b1", "name": "Out", "file": "../outside.wav"})";
  files["assets/Gone.json"] = R"({"type": "asset", "guid": "00000000-0000-4000-8000-0000000000b2", "name": "Gone", "file": "wav/none.wav"})";
  WriteBaseProject(dir, files);
  WriteBytes(root + "/outside.wav", MakeWav(48000, 100));
  WriteFile(dir + "/buses/blob.bin", "BLOB");
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.Set(kSfx, "effects[0].blob", String("buses/blob.bin")));
  const std::string other = root + "/copy";
  // The target already holds stale versions of files the copy writes.
  WriteFile(other + "/buses/SFX.json", "stale");
  WriteFile(other + "/wav/tone.wav", "stale");
  SND_CHECK(doc.SaveTo(other));
  const SaveResult& r = doc.lastSave();
  SND_CHECK(r.ok);
  bool out = false, missing = false;
  for (const auto& f : r.outside) out |= f == "../outside.wav";
  for (const auto& f : r.missing) missing |= f == "wav/none.wav";
  SND_CHECK(out);
  SND_CHECK(missing);
  SND_CHECK_EQ(doc.directory(), other);
  SND_CHECK(!doc.IsDirty());
  const auto c = DirContents(other);
  SND_CHECK(c.count("buses/blob.bin") == 1 && c.at("buses/blob.bin") == "BLOB");
  SND_CHECK(c.count("wav/tone.wav") == 1 && c.at("wav/tone.wav") == ReadFileText(dir + "/wav/tone.wav"));
  SND_CHECK(c.count("buses/SFX.json") == 1 && c.at("buses/SFX.json") == doc.TextOf(*doc.Find(kSfx)));
  SND_CHECK(c.count("outside.wav") == 0);
  SND_CHECK(!AnyTempFile(other));
  // The copy loads back to the same objects.
  ProjectDocument back;
  SND_REQUIRE(back.Load(other));
  SND_CHECK(Image(back) == Image(doc));
}

SND_TEST(IndepS7Recheck, R2_SaveAsCopyFailureLeavesTheDocumentWhereItWas) {
  const std::string root = TempDir("r2f");
  const std::string dir = root + "/proj";
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.SetNumber(kSfx, "volumeDb", -4));
  const std::string other = root + "/copy";
  std::error_code ec;
  fs::create_directories(other + "/wav/tone.wav/block", ec);
  SND_CHECK(!doc.SaveTo(other));
  SND_CHECK(!doc.lastSave().ok);
  bool named = false;
  for (const auto& f : doc.lastSave().failures) named |= f.path == "wav/tone.wav";
  SND_CHECK(named);
  SND_CHECK_EQ(doc.directory(), dir);
  SND_CHECK(doc.IsDirty());  // the original directory was not saved
  SND_CHECK(doc.Save());     // and can still be
  SND_CHECK_EQ(NumberAt(doc.Find(kSfx)->doc, "volumeDb", 0), -4.0);
}

SND_TEST(IndepS7Recheck, R2_SaveToTheSameDirectoryIsSave) {
  const std::string dir = TempDir("r2s");
  WriteBaseProject(dir);
  ProjectDocument doc;
  SND_REQUIRE(doc.Load(dir));
  SND_REQUIRE(doc.Remove(kCatUi));
  SND_REQUIRE(doc.Rename(kReverb, "Hall"));
  SND_CHECK(doc.SaveTo(dir));
  SND_CHECK(!fs::exists(dir + "/categories/UI.json"));
  SND_CHECK(!fs::exists(dir + "/buses/Reverb.json"));
  SND_CHECK(!doc.IsDirty());
  ProjectDocument back;
  SND_REQUIRE(back.Load(dir));
  SND_CHECK(back.loadIssues().empty());
  SND_CHECK(Image(back) == Image(doc));
}

// --- R3: reserved names (D-176f) ---------------------------------------------------------------------------

SND_TEST(IndepS7Recheck, R3_WindowsReservedNamesGetAPrefix) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const struct {
    const char* name;
    const char* file;
  } cases[] = {{"CON", "events/_CON.json"},     {"con", "events/_con.json"},     {"Prn", "events/_Prn.json"},
               {"aux", "events/_aux.json"},     {"NUL", "events/_NUL.json"},     {"COM1", "events/_COM1.json"},
               {"com9", "events/_com9.json"},   {"LPT1", "events/_LPT1.json"},   {"lpt9", "events/_lpt9.json"},
               {"NUL.x", "events/_NUL.x.json"}, {"COM10", "events/COM10.json"}, {"CONSOLE", "events/CONSOLE.json"},
               {"COM0", "events/COM0.json"},    {"XCON", "events/XCON.json"},    {"SE/CON", "events/SE/_CON.json"}};
  for (const auto& c : cases) {
    const std::string got = doc.FileNameFor("event", c.name);
    if (got != c.file) {
      snd::test::Note("FileNameFor(%s) = %s, expected %s", c.name, got.c_str(), c.file);
      SND_CHECK(false);
    }
  }
}

// --- R4: same value = success, no step, not dirty (D-177b) ------------------------------------------------

SND_TEST(IndepS7Recheck, R4_SameValueSettersAreTrueWithoutAStep) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  std::vector<std::pair<const char*, std::function<bool()>>> ops;
  EventEditor shot(doc, kShot), engine(doc, kEngine);
  MixerEditor mx(doc);
  CurveEditor curve(doc, kShot, "curves[0]");
  ParameterEditor speed(doc, kSpeed);
  SegmentEditor seg(doc, kSegA);
  PlaylistEditor pl(doc, kPlExplore);
  MusicEditor music(doc, kBgm);
  MaterialEditor mat(doc, kPhys);
  PatchEditor patch(doc, kPatch);
  ops.push_back({"doc.SetNumber", [&] { return doc.SetNumber(kSfx, "volumeDb", -3); }});
  ops.push_back({"doc.Set string", [&] { return doc.Set(kShot, "sequence", String("random")); }});
  ops.push_back({"Event priority", [&] { return shot.SetNumber(EventNumber::Priority, 100); }});
  ops.push_back({"Event retrigger", [&] { return shot.SetNumber(EventNumber::RetriggerMs, 50); }});
  ops.push_back({"Event maxInstances", [&] { return shot.SetNumber(EventNumber::MaxInstances, 4); }});
  ops.push_back({"Event sequence", [&] { return shot.SetSequence(SequenceKind::Random); }});
  ops.push_back({"Event bus", [&] { return shot.SetBus(kSfx); }});
  ops.push_back({"Event category", [&] { return shot.SetCategory(kCatSfx); }});
  ops.push_back({"Event limitGroup", [&] { return shot.SetLimitGroup(kGroup); }});
  ops.push_back({"Clip startMs", [&] { return shot.SetClipNumber(0, 0, ClipNumber::StartMs, 0); }});
  ops.push_back({"Clip weight", [&] { return shot.SetClipNumber(0, 0, ClipNumber::Weight, 2); }});
  ops.push_back({"Clip random", [&] { return shot.SetClipRandom(0, 0, RandomField::VolumeDb, 2); }});
  ops.push_back({"MoveClip", [&] { return shot.MoveClip(0, 1, 100); }});
  ops.push_back({"Action", [&] { return shot.SetAction(0, 0, shot.Action(0, 0)); }});
  ops.push_back({"Cone", [&] { return engine.SetCone(90, 180, -12); }});
  ops.push_back({"Flag", [&] { return engine.SetFlag(EventFlag::Is3D, true); }});
  ops.push_back({"Mixer volume", [&] { return mx.SetVolume(kSfx, -3); }});
  ops.push_back({"Mixer parent", [&] { return mx.SetParent(kSfx, kMaster); }});
  ops.push_back({"Send level", [&] { return mx.SetSendLevel(kSfx, 0, -12); }});
  ops.push_back({"Send target", [&] { return mx.SetSendTarget(kSfx, 0, kReverb); }});
  ops.push_back({"Effect param", [&] { return mx.SetEffectParam(kSfx, 0, 0, 8000); }});
  ops.push_back({"Category volume", [&] { return mx.SetCategoryVolume(kCatSfx, -2.5); }});
  ops.push_back({"Category flags", [&] { return mx.SetCategoryFlag(kCatUi, true, false); }});
  ops.push_back({"Max voices", [&] { return mx.SetMaxVoices(kGroup, 8); }});
  ops.push_back({"Steal", [&] { return mx.SetSteal(kGroup, StealMode::Oldest); }});
  ops.push_back({"Ducking amount", [&] { return mx.SetDucking(kDuck, DuckField::AmountDb, -6); }});
  ops.push_back({"Ducking attack", [&] { return mx.SetDucking(kDuck, DuckField::AttackMs, 20); }});
  ops.push_back({"Ducking kind", [&] { return mx.SetDuckingKind(kDuck, DuckKind::Sidechain, kMusicBus); }});
  ops.push_back({"Ducking target", [&] { return mx.SetDuckingTarget(kDuck, kSfx); }});
  ops.push_back({"Curve point", [&] { return curve.MovePoint(1, 10, -12); }});
  ops.push_back({"Curve target", [&] { return curve.SetTarget(CurveTarget::Volume); }});
  ops.push_back({"Curve parameter", [&] { return curve.SetParameter(kSpeed); }});
  ops.push_back({"Curve points", [&] { return curve.SetPoints(curve.Points()); }});
  ops.push_back({"Param scope", [&] { return speed.SetScope(3); }});
  ops.push_back({"Param range", [&] { return speed.SetRange(0, 10); }});
  ops.push_back({"Param default", [&] { return speed.SetDefault(2); }});
  ops.push_back({"Param ramp", [&] { return speed.SetRampRate(5); }});
  ops.push_back({"Segment bars", [&] { return seg.SetBars(4); }});
  ops.push_back({"Segment asset", [&] { return seg.SetAsset(kStream); }});
  ops.push_back({"Segment tempo", [&] { return seg.SetTempo(0, seg.Tempo()[0]); }});
  ops.push_back({"Playlist entry", [&] { return pl.SetEntry(0, kSegA, 1, 1); }});
  ops.push_back({"Music initial", [&] { return music.SetInitialState("Explore"); }});
  ops.push_back({"Music state playlist", [&] { return music.SetStatePlaylist("Explore", kPlExplore); }});
  ops.push_back({"Material single", [&] { return mat.SetSingle("metal", ContactKind::Impact, kShot); }});
  ops.push_back({"Material pair", [&] { return mat.SetPair("glass", "metal", kSting); }});
  ops.push_back({"Material footstep", [&] { return mat.SetFootstep("wood", kHit); }});
  ops.push_back({"Material default", [&] { return mat.SetDefault(ContactKind::Impact, kEngine); }});
  ops.push_back({"Patch move", [&] { return patch.MoveNode("osc", 100, 0); }});
  ops.push_back({"Patch param", [&] { return patch.SetParam("osc", "frequencyHz", 220); }});
  ops.push_back({"Patch choice", [&] { return patch.SetChoice("osc", "waveform", "saw"); }});
  // (SetSendPreFader, SetSwitch, SetMarker, SetRule with the current value and MoveMaterial(i, i):
  // Defect_SameValueSettersRewriteExplicitDefaults in indep_s7_defects_test.cpp.)
  const auto image = Image(doc);
  for (auto& [name, fn] : ops) {
    const bool ok = fn();
    const bool clean = doc.UndoCount() == 0u && doc.PendingChanges().empty() && Image(doc) == image;
    if (!ok || !clean) {
      snd::test::Note("%s: returned %d, %s", name, ok ? 1 : 0, clean ? "no change" : "CHANGED the document / history");
      SND_CHECK(false);
      while (doc.Undo()) {
      }
    }
  }
}

// --- R5: non-finite numbers refused everywhere (D-176c) ------------------------------------------------

SND_TEST(IndepS7Recheck, R5_NonFiniteNumbersAreRefusedOnEveryPath) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const auto image = Image(doc);
  std::vector<std::pair<std::string, std::function<bool()>>> ops;
  EventEditor shot(doc, kShot), engine(doc, kEngine);
  MixerEditor mx(doc);
  CurveEditor curve(doc, kShot, "curves[0]");
  ParameterEditor speed(doc, kSpeed);
  SegmentEditor seg(doc, kSegA);
  PlaylistEditor pl(doc, kPlExplore);
  MusicEditor music(doc, kBgm);
  PatchEditor patch(doc, kPatch);
  for (double bad : {kNaN, kInf, -kInf}) {
    const std::string tag = std::isnan(bad) ? "NaN" : (bad > 0 ? "+Inf" : "-Inf");
    ops.push_back({"Set " + tag, [&doc, bad] { return doc.Set(kSfx, "volumeDb", Number(bad)); }});
    ops.push_back({"SetNumber new member " + tag, [&doc, bad] { return doc.SetNumber(kSfx, "lodPriority", bad); }});
    ops.push_back({"Set nested " + tag, [&doc, bad] {
                     json::Value cone = json::MakeObject();
                     json::Add(cone, "innerDeg", Number(10));
                     json::Add(cone, "outerDeg", Number(bad));
                     return doc.Set(kEngine, "cone", cone);
                   }});
    ops.push_back({"Insert " + tag, [&doc, bad] {
                     json::Value p = json::MakeArray();
                     p.items.push_back(Number(5));
                     p.items.push_back(Number(bad));
                     return doc.Insert(kShot, "curves[0].points", 1, p);
                   }});
    ops.push_back({"Modify " + tag, [&doc, bad] {
                     return doc.Modify(kShot, [bad](json::Value& v) {
                       SetMember(v, "fadeOutMs", Number(bad), nullptr);
                       return true;
                     });
                   }});
    ops.push_back({"Create init " + tag, [&doc, bad] {
                     json::Value init = json::MakeObject();
                     json::Add(init, "volumeDb", Number(bad));
                     json::Add(init, "parent", String(kMaster));
                     return !doc.Create("bus", "BadBus", &init).empty();
                   }});
    ops.push_back({"Event number " + tag, [&shot, bad] { return shot.SetNumber(EventNumber::FadeOutMs, bad); }});
    ops.push_back({"Clip volume " + tag, [&shot, bad] { return shot.SetClipNumber(0, 0, ClipNumber::VolumeDb, bad); }});
    ops.push_back({"Clip pitch " + tag, [&shot, bad] { return shot.SetClipNumber(0, 0, ClipNumber::PitchCents, bad); }});
    ops.push_back({"Clip random " + tag, [&shot, bad] { return shot.SetClipRandom(0, 0, RandomField::Pan, bad); }});
    ops.push_back({"Add clip " + tag, [&shot, bad] { return shot.AddAssetClip(0, kTone, bad); }});
    ops.push_back({"Action " + tag, [&shot, bad] {
                     ActionDesc a = shot.Action(0, 0);
                     a.value = bad;
                     return shot.SetAction(0, 0, a);
                   }});
    ops.push_back({"Synth " + tag, [&shot, bad] {
                     SynthDesc s;
                     s.frequencyHz = bad;
                     return shot.AddSynthClip(0, s, 0);
                   }});
    ops.push_back({"Mixer volume " + tag, [&mx, bad] { return mx.SetVolume(kSfx, bad); }});
    ops.push_back({"Send " + tag, [&mx, bad] { return mx.SetSendLevel(kSfx, 0, bad); }});
    ops.push_back({"Add send " + tag, [&mx, bad] { return mx.AddSend(kMusicBus, kReverb, bad); }});
    ops.push_back({"Effect " + tag, [&mx, bad] { return mx.SetEffectParam(kSfx, 0, 0, bad); }});
    ops.push_back({"Category " + tag, [&mx, bad] { return mx.SetCategoryVolume(kCatSfx, bad); }});
    ops.push_back({"Ducking " + tag, [&mx, bad] { return mx.SetDucking(kDuck, DuckField::ThresholdDb, bad); }});
    ops.push_back({"Snapshot " + tag, [&mx, bad] {
                     SnapshotValue v;
                     v.bus = kSfx;
                     v.value = bad;
                     return mx.SetSnapshotValue(kSnap, 0, v);
                   }});
    ops.push_back({"Curve point " + tag, [&curve, bad] { return curve.MovePoint(1, 10, bad); }});
    ops.push_back({"Curve add " + tag, [&curve, bad] { return curve.AddPoint(bad, 1) >= 0; }});
    ops.push_back({"Distance curve " + tag, [&doc, bad] {
                     return SetDistanceCurve(doc, kEngine, CurveTarget::Volume, {{0, 0, 0}, {10, bad, 0}});
                   }});
    ops.push_back({"Param range " + tag, [&speed, bad] { return speed.SetRange(bad, 10); }});
    ops.push_back({"Param ramp " + tag, [&speed, bad] { return speed.SetRampRate(bad); }});
    ops.push_back({"Tempo " + tag, [&seg, bad] {
                     TempoPointDesc t = seg.Tempo()[0];
                     t.bpm = bad;
                     return seg.SetTempo(0, t);
                   }});
    ops.push_back({"Grid " + tag, [&seg, bad] { return seg.ApplyGrid(bad, 0, 4, 4); }});
    ops.push_back({"Playlist " + tag, [&pl, bad] { return pl.SetEntry(0, kSegA, bad, 1); }});
    ops.push_back({"Rule " + tag, [&music, bad] {
                     TransitionRule r = music.Rules()[0];
                     r.fadeInBeats = bad;
                     return music.SetRule(0, r);
                   }});
    ops.push_back({"Patch param " + tag, [&patch, bad] { return patch.SetParam("osc", "frequencyHz", bad); }});
    ops.push_back({"Patch move " + tag, [&patch, bad] { return patch.MoveNode("osc", static_cast<float>(bad), 0); }});
  }
  for (auto& [name, fn] : ops) {
    const bool ok = fn();
    if (ok || Image(doc) != image || doc.UndoCount() != 0u) {
      snd::test::Note("%s: returned %d, document %s", name.c_str(), ok ? 1 : 0, Image(doc) == image ? "unchanged" : "CHANGED");
      SND_CHECK(false);
      while (doc.Undo()) {
      }
    }
  }
}

// --- R6: GUIDs, editor rules (D-177c, d) ------------------------------------------------------------------

SND_TEST(IndepS7Recheck, R6_StemMaterialUnreadableIsAllowedAndListed) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  json::Value init = json::MakeObject();
  json::Add(init, "file", String("wav/absent.wav"));
  const std::string absent = doc.Create("asset", "Absent", &init);
  SND_REQUIRE(!absent.empty());
  SegmentEditor s(doc, kSegA);
  SND_CHECK(s.Problems().empty());
  StemDesc st;
  st.channelFirst = 5;
  st.channelCount = 3;
  st.asset = absent;
  SND_CHECK_EQ(s.StemChannels(absent), -1);
  SND_CHECK_EQ(s.StemChannels(""), 2);
  SND_CHECK_EQ(s.StemChannels(kTone), 1);
  SND_CHECK(s.AddStem(st));  // not refused: the material cannot be read
  const auto p = s.Problems();
  SND_REQUIRE(p.size() == 1u);
  SND_CHECK(p[0].find("stems[0]") != std::string::npos);
  // Stems inside: accepted at the exact edge.
  StemDesc edge;
  edge.channelFirst = 1;
  edge.channelCount = 1;
  SND_CHECK(s.AddStem(edge));
  edge.channelCount = 0;
  SND_CHECK(!s.AddStem(edge));
  // 2^32 overflow of first + count must not wrap into range.
  StemDesc wrap;
  wrap.channelFirst = 0xFFFFFFFFu;
  wrap.channelCount = 2;
  SND_CHECK(!s.AddStem(wrap));
}

// --- R7: computation like the runtime (D-179c, e) ---------------------------------------------------------

SND_TEST(IndepS7Recheck, D179c_MidBarChangeIsTheRuntimes) {
  auto files = BaseFiles();
  files["segments/Seg/Odd.json"] = std::string(R"({"type": "segment", "guid": "00000000-0000-4000-8000-0000000000b3", "name": "Seg/Odd", "asset": ")") +
                                   kMusicAsset + R"(", "bars": 4, "tempo": [{"tick": 0, "bpm": 120}, {"tick": 5000, "bpm": 90, "beatsPerBar": 3, "beatUnit": 4}, {"tick": 9001, "bpm": 140, "beatsPerBar": 7, "beatUnit": 8}]})";
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc, files));
  SegmentEditor s(doc, "00000000-0000-4000-8000-0000000000b3");
  SND_CHECK(!s.Problems().empty());  // the loaded mid-bar changes are listed
  // The build's diagnostics are not changed by D-179c: no error.
  std::uint32_t errors = 0;
  for (const auto& i : CheckDocument(doc)) errors += i.error ? 1u : 0u;
  SND_CHECK_EQ(errors, 0u);
  const TempoMap tool = s.Map();
  const auto& pts = tool.points();
  snd::music::detail::Timeline tl;
  tl.Reset(48000.0, pts[0].bpm, static_cast<std::uint16_t>(pts[0].beatsPerBar), static_cast<std::uint16_t>(pts[0].beatUnit));
  for (std::size_t i = 1; i < pts.size(); ++i)
    SND_REQUIRE(tl.AddPoint(pts[i].tick, pts[i].bpm, static_cast<std::uint16_t>(pts[i].beatsPerBar),
                            static_cast<std::uint16_t>(pts[i].beatUnit)));
  tl.StartAt(0, 0.0);
  for (double tick = 0; tick < 20000; tick += 61.5) {
    SND_CHECK_NEAR(tl.BarPosition(tick), tool.BarPosition(tick), 1e-12);
    const snd_musical_time a = tl.Musical(tick);
    const MusicalPosition b = tool.Musical(tick);
    SND_CHECK(a.bar == b.bar && a.beat == b.beat && a.tick == b.tick);
    SND_CHECK_NEAR(tl.TickOfMusical(a), tool.TickOfMusical(b), 1e-9);
    SND_CHECK_NEAR(tl.NextBar(tick), tool.NextBar(tick), 1e-9);
    SND_CHECK_NEAR(tl.NextBeat(tick), tool.NextBeat(tick), 1e-9);
    double clock = 0;
    SND_REQUIRE(tl.ClockAtTick(tick, &clock));
    SND_CHECK_NEAR(clock / 48000.0, tool.SecondsAtTick(tick), 1e-9);
  }
  // Editing such a segment: a change still refused where the signature before it has no bar line,
  // and editing an unrelated member is possible.
  SND_CHECK(s.SetBars(5));
}

SND_TEST(IndepS7Recheck, D179e_AutoSortRefusedWithUnreachableRows) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MusicEditor me(doc, kBgm);
  TransitionRule r;
  r.from = "Explore";
  r.to = "Battle";
  r.timing = Timing::Immediate;
  SND_REQUIRE(me.AddRule(r, 2));  // after *->*: unreachable
  SND_REQUIRE(!me.Unreachable().empty());
  const auto image = Image(doc);
  const std::size_t steps = doc.UndoCount();
  std::string why;
  SND_CHECK(!me.AutoSort(&why));
  SND_CHECK(!why.empty());
  SND_CHECK(!me.AutoSort());  // null reason pointer is allowed
  SND_CHECK(Image(doc) == image);
  SND_CHECK_EQ(doc.UndoCount(), steps);
  SND_REQUIRE(me.RemoveRule(2));
  SND_CHECK(me.AutoSort(&why));
}

// --- R8: the member table covers the format; Column answers are the bank (D-178b, c) -------------------------

SND_TEST(IndepS7Recheck, R8_EverySchemaMemberHasAnEffect) {
  const auto kinds = snd::buildtool::SchemaKinds();
  SND_CHECK(kinds.size() >= 14u);
  std::uint32_t members = 0;
  for (const auto& k : kinds) {
    for (const auto& m : snd::buildtool::SchemaMembers(k)) {
      ++members;
      if (ClassifyMember(k, m) == FieldEffect::Unknown) {
        snd::test::Note("%s.%s: no effect", k.c_str(), m.c_str());
        SND_CHECK(false);
      }
    }
    // notes anywhere: NotInBank.
    SND_CHECK(ClassifyMember(k, "notes") == FieldEffect::NotInBank);
  }
  SND_CHECK(members > 200u);
  SND_CHECK(ClassifyMember("event", "tracks[3].clips[2].notes") == FieldEffect::NotInBank);
  SND_CHECK(ClassifyMember("event", "tracks[3].clips[2].notes.deep[1]") == FieldEffect::NotInBank);
  SND_CHECK(ClassifyMember("patch", "nodes[0].position[1]") == FieldEffect::NotInBank);
  SND_CHECK(ClassifyMember("event", "noSuchMember") == FieldEffect::Unknown);
  SND_CHECK(ClassifyMember("nonsense", "volumeDb") == FieldEffect::Unknown);
  SND_CHECK(ClassifyMember("bus", "name") == FieldEffect::Structure);
  // Every Column entry of MemberTable is in FieldColumns and the contract's columns.
  std::uint32_t columns = 0;
  for (const auto& f : MemberTable()) columns += f.effect == FieldEffect::Column ? 1u : 0u;
  SND_CHECK_EQ(static_cast<std::size_t>(columns), FieldColumns().size());
}

SND_TEST(IndepS7Recheck, R8_ColumnMembersMatchTheBankAndDiffBuilds) {
  ProjectDocument base;
  SND_REQUIRE(LoadBase(base));
  // Every distance curve and a stem curve too, so that those rows exist.
  SND_REQUIRE(SetDistanceCurve(base, kEngine, CurveTarget::Volume, {{0, 0, 0}, {50, -30, 1}}));
  SND_REQUIRE(SetDistanceCurve(base, kEngine, CurveTarget::Lowpass, {{0, 20000, 0}, {50, 2000, 0}}));
  SegmentEditor seg(base, kSegA);
  StemDesc st;
  st.channelFirst = 0;
  st.channelCount = 2;
  SND_REQUIRE(seg.AddStem(st));
  base.ClearHistory();
  base.MarkSaved();
  const DocBuild b0 = BuildDocument(base);
  SND_REQUIRE(b0.ok);
  std::uint32_t checked = 0, valueEdits = 0, mismatches = 0;
  for (const auto& o : base.objects()) {
    std::vector<std::pair<std::string, const json::Value*>> leaves;
    Leaves(o.doc, "", &leaves);
    const std::string kind = o.kind();
    for (const auto& [path, v] : leaves) {
      const FieldEffect e = ClassifyMember(kind, path);
      if (e == FieldEffect::Unknown) {
        snd::test::Note("%s %s: Unknown", kind.c_str(), path.c_str());
        SND_CHECK(false);
        continue;
      }
      if (e != FieldEffect::Column) continue;
      const auto cells = LocateField(b0, base, o.guid, path);
      // The cells hold the bank's bits; a number member equals its cell.
      for (const auto& c : cells) {
        const BankImage* img = b0.FindBank(c.bank, c.language);
        SND_REQUIRE(img != nullptr && img->view() != nullptr);
        std::uint64_t bits = 0;
        SND_CHECK(img->view()->Cell(c.chunk, c.column, c.row, &bits));
        SND_CHECK_EQ(bits, c.bits);
        if (c.liveWritable) SND_CHECK(img->view()->HasColumn(c.chunk, c.column));
        double dv = 0;
        if (v->IsNumber() && v->AsDouble(&dv)) {
          const double got = c.type == CellType::F32 ? static_cast<double>(BitsToFloat(bits)) : static_cast<double>(bits);
          const double want = c.type == CellType::F32 ? static_cast<double>(static_cast<float>(dv)) : dv;
          // Cells that hold a converted value (ms -> frames, names -> ids) are not compared.
          if (c.type == CellType::F32 && got != want) {
            snd::test::Note("%s %s: bank %g, document %g", o.name().c_str(), path.c_str(), got, dv);
            SND_CHECK(false);
          }
        }
        ++checked;
      }
      // DiffBuilds of an edit of this member agrees with LocateField (numbers only).
      double dv = 0;
      if (!v->IsNumber() || !v->AsDouble(&dv) || cells.empty()) continue;
      ProjectDocument d = base;
      const double nv = dv + (std::fabs(dv) > 1000 ? 7.0 : 0.25);
      if (!d.SetNumber(o.guid, path, nv)) continue;
      const DocBuild b1 = BuildDocument(d);
      if (!b1.ok) continue;
      const LiveDelta delta = DiffBuilds(b0, b1);
      const bool live = cells[0].liveWritable && cells[0].addressable;
      if (delta.kind == EditClass::Value) {
        ++valueEdits;
        for (const auto& ve : delta.values) {
          bool found = false;
          for (const auto& c : LocateField(b1, d, o.guid, path))
            found |= c.chunk == ve.cell.chunk && c.column == ve.cell.column && c.row == ve.cell.row && c.liveWritable &&
                     c.runtimeId == ve.cell.runtimeId && c.index == ve.cell.index;
          if (!found) {
            snd::test::Note("%s %s: DiffBuilds edits a cell LocateField does not give", o.name().c_str(), path.c_str());
            SND_CHECK(false);
            ++mismatches;
          }
        }
      } else if (live && delta.kind == EditClass::Structure && delta.chunks.size() == 0u) {
        snd::test::Note("%s %s: live cell but a structure edit without chunks", o.name().c_str(), path.c_str());
        SND_CHECK(false);
        ++mismatches;
      } else if (live && (std::fabs(nv - dv) > 0) && delta.kind == EditClass::Structure) {
        // A live cell whose value edit DiffBuilds calls structural: only when the value does not
        // reach the bank as a plain change of that cell (other cells / chunks changed too).
        bool onlyThatChunk = true;
        for (const auto& ch : delta.chunks) onlyThatChunk &= ch.fourcc == cells[0].chunk;
        if (onlyThatChunk && delta.values.empty()) {
          snd::test::Note("%s %s: LocateField says live, DiffBuilds replaces the chunk", o.name().c_str(), path.c_str());
          SND_CHECK(false);
          ++mismatches;
        }
      }
    }
  }
  snd::test::Note("checked %u cells, %u value edits, %u mismatches", checked, valueEdits, mismatches);
  SND_CHECK(checked > 60u);
  SND_CHECK(valueEdits > 20u);
}

SND_TEST(IndepS7Recheck, R8_NotInBankEditsChangeNothing) {
  ProjectDocument base;
  SND_REQUIRE(LoadBase(base));
  const DocBuild b0 = BuildDocument(base);
  SND_REQUIRE(b0.ok);
  const char* const cases[][2] = {{kShot, "notes"}, {kShot, "tracks[0].notes"}, {kShot, "tracks[0].clips[0].notes"},
                                  {kSfx, "notes"},  {kPhys, "single.notes"},   {kBgm, "music.transitions[0].notes"}};
  for (const auto& c : cases) {
    SND_CHECK(ClassifyMember(base.Find(c[0])->kind(), c[1]) == FieldEffect::NotInBank);
    ProjectDocument d = base;
    SND_REQUIRE(d.Set(c[0], c[1], String("x")));
    SND_CHECK(DiffBuilds(b0, BuildDocument(d)).kind == EditClass::None);
  }
  ProjectDocument d = base;
  PatchEditor p(d, kPatch);
  SND_REQUIRE(p.MoveNode("env", 3, 4));
  SND_CHECK(ClassifyMember("patch", "nodes[1].position[0]") == FieldEffect::NotInBank);
  SND_CHECK(DiffBuilds(b0, BuildDocument(d)).kind == EditClass::None);
}
